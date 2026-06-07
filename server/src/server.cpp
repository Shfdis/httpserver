#include "server.h"
#include "http_error.h"
#include "request_data.h"
#include "trie.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <ctime>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace HTTP {

namespace {
bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string trim_ows(std::string_view s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
    --end;
  }
  return std::string(s.substr(start, end - start));
}

bool has_token(std::string_view value, std::string_view token) {
  size_t pos = 0;
  while (pos <= value.size()) {
    const size_t comma = value.find(',', pos);
    const size_t end = comma == std::string_view::npos ? value.size() : comma;
    if (iequals(trim_ows(value.substr(pos, end - pos)), token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    pos = comma + 1;
  }
  return false;
}

bool ShouldKeepAlive(const RequestData &request) {
  const auto connection = request.Header("Connection");
  if (request.version == "HTTP/1.0") {
    return connection && has_token(*connection, "keep-alive");
  }
  if (connection && has_token(*connection, "close")) {
    return false;
  }
  return request.version == "HTTP/1.1";
}

bool is_tchar(unsigned char ch) {
  if (std::isalnum(ch)) {
    return true;
  }
  switch (ch) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '{':
  case '|':
  case '}':
  case '~':
    return true;
  default:
    return false;
  }
}

bool valid_header_name(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char ch) {
    return is_tchar(static_cast<unsigned char>(ch));
  });
}

bool valid_header_value(std::string_view value) {
  for (unsigned char ch : value) {
    if (ch == '\t') {
      continue;
    }
    if (ch < 0x20 || ch == 0x7f) {
      return false;
    }
  }
  return true;
}

bool reserved_response_header(std::string_view name) {
  return iequals(name, "Content-Length") ||
         iequals(name, "Transfer-Encoding") || iequals(name, "Connection") ||
         iequals(name, "Date");
}

unsigned short NormalizeStatus(unsigned short status) {
  if (status < 100 || status > 599) {
    return 500;
  }
  return status;
}

std::string_view ReasonPhrase(unsigned short status) {
  switch (status) {
  case 100:
    return "Continue";
  case 101:
    return "Switching Protocols";
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 304:
    return "Not Modified";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 413:
    return "Content Too Large";
  case 414:
    return "URI Too Long";
  case 417:
    return "Expectation Failed";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 505:
    return "HTTP Version Not Supported";
  default:
    if (status < 200) {
      return "Informational";
    }
    if (status < 300) {
      return "Successful";
    }
    if (status < 400) {
      return "Redirection";
    }
    if (status < 500) {
      return "Client Error";
    }
    return "Server Error";
  }
}

std::string HttpDate() {
  std::time_t now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&now, &tm);
  char buffer[64]{};
  std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return buffer;
}

bool StatusAllowsBody(unsigned short status) {
  return status >= 200 && status != 204 && status != 304;
}

bool ShouldSendBody(unsigned short status, Method method) {
  return method != HEAD && StatusAllowsBody(status);
}

constexpr int kMaxIoAttempts = 3;

bool IsRetryableIoError(int result) {
  return result == -EINTR || result == -EAGAIN || result == -EWOULDBLOCK;
}
} // namespace

Server::Server(Server &&rhs) {
  trie_ = std::move(rhs.trie_);
  socketFD_ = rhs.socketFD_;
  port_ = rhs.port_;
  numThreads_ = rhs.numThreads_;
  stopFlag_.store(rhs.stopFlag_.load());
  workerFutures_ = std::move(rhs.workerFutures_);
  serverLoop_ = std::move(rhs.serverLoop_);
  rhs.socketFD_ = -1;
}

Server::~Server() {
  stopFlag_ = true;
  if (socketFD_ != -1) {
    shutdown(socketFD_, SHUT_RDWR);
    close(socketFD_);
    socketFD_ = -1;
  }
  for (auto &future : workerFutures_) {
    future.Get();
  }
}

CoFuture<void> Server::AcceptAndProcess(IOUring &ring) {
  while (!stopFlag_.load()) {
    int connectionFD = co_await ring.AcceptAsync(socketFD_);

    if (connectionFD < 0) {
      if (stopFlag_.load()) {
        co_return;
      }
      continue;
    }

    Process(ring, connectionFD);
  }
  co_return;
}

void Server::WorkerLoop(IOUring &ring) {
  try {
    auto acceptCoro = AcceptAndProcess(ring);
    
    while (!stopFlag_.load()) {
      ring.Poll();
      
      if (acceptCoro.isReady()) {
        acceptCoro = AcceptAndProcess(ring);
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[WorkerLoop] Exception: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[WorkerLoop] Unknown exception" << std::endl;
  }
}

CoFuture<void> Server::WriteRaw(IOUring &ring, int connectionFD,
                                std::string_view data) {
  size_t sent = 0;
  int attempts = 0;
  while (sent < data.size()) {
    int result =
        co_await ring.WriteAsync(connectionFD, data, sent, data.size() - sent);
    if (result > 0) {
      sent += static_cast<size_t>(result);
      attempts = 0;
      continue;
    }
    if (result == 0) {
      break;
    }
    ++attempts;
    if (!IsRetryableIoError(result) || attempts >= kMaxIoAttempts) {
      throw std::system_error(-result, std::generic_category(), "write failed");
    }
  }
  co_return;
}

CoFuture<void> Server::WriteResponse(IOUring &ring, int connectionFD,
                                     const ResponseData &data,
                                     const RequestData &request,
                                     bool keepAlive) {
  const unsigned short status = NormalizeStatus(data.status);
  std::stringstream text;
  text << "HTTP/1.1 " << status << ' ' << ReasonPhrase(status) << "\r\n";
  text << "Date: " << HttpDate() << "\r\n";

  if (StatusAllowsBody(status)) {
    text << "Content-Length: " << data.body.size() << "\r\n";
  }

  if (!keepAlive) {
    text << "Connection: close\r\n";
  } else if (request.version == "HTTP/1.0") {
    text << "Connection: keep-alive\r\n";
  }

  for (const auto &[name, value] : data.headers) {
    if (reserved_response_header(name) || !valid_header_name(name) ||
        !valid_header_value(value)) {
      continue;
    }
    text << name << ": " << value << "\r\n";
  }
  text << "\r\n";
  if (ShouldSendBody(status, request.method)) {
    text << data.body;
  }

  std::string final = text.str();
  co_await WriteRaw(ring, connectionFD, final);
  co_return;
}

CoFuture<void> Server::Process(IOUring &ring, int connectionFD) {
  HttpRequestParser parser(ring, connectionFD);

  while (true) {
    RequestData request;
    ResponseData response;
    bool keepAlive = true;
    bool mustClose = false;

    try {
      while (true) {
        RequestReadStatus readStatus = co_await parser.ReadRequest(request);
        if (readStatus == RequestReadStatus::Closed) {
          (void)shutdown(connectionFD, SHUT_WR);
          close(connectionFD);
          co_return;
        }
        if (readStatus == RequestReadStatus::Complete) {
          break;
        }
        co_await WriteRaw(ring, connectionFD,
                          "HTTP/1.1 100 Continue\r\n\r\n");
        parser.MarkContinueSent();
      }

      keepAlive = ShouldKeepAlive(request);
      if (request.method == UNKNOWN) {
        response.status = 501;
        response.body = "Not Implemented";
      } else if (request.method == OPTIONS && request.path == "*") {
        response.status = 200;
        response.headers["Allow"] = trie_.AllAllowedMethods();
      } else {
        Trie::RouteResult route =
            trie_.Resolve(request.method, request.path, request.urlVariables);
        if (!route.pathFound) {
          response.status = 404;
          response.body = "Not Found";
        } else if (request.method == OPTIONS && route.automaticOptions) {
          response.status = 200;
          response.headers["Allow"] = route.allow;
        } else if (!route.methodAllowed) {
          response.status = 405;
          response.body = "Method Not Allowed";
          response.headers["Allow"] = route.allow;
        } else {
          response = route.handler(request);
        }
      }
    } catch (HTTPError &error) {
      response.status = error.status;
      response.body = error.message;
      mustClose = true;
      keepAlive = false;
    } catch (const std::system_error &) {
      response.status = 500;
      response.body = "Internal server error";
      mustClose = true;
      keepAlive = false;
    } catch (std::runtime_error &error) {
      response.status = 500;
      response.body = error.what();
      mustClose = true;
      keepAlive = false;
    } catch (...) {
      response.status = 500;
      response.body = "Internal server error";
      mustClose = true;
      keepAlive = false;
    }

    if (!mustClose || response.status != 400 || !response.body.empty()) {
      try {
        co_await WriteResponse(ring, connectionFD, response, request, keepAlive);
      } catch (const std::system_error &) {
        mustClose = true;
        keepAlive = false;
      }
    }
    if (!keepAlive || mustClose) {
      (void)shutdown(connectionFD, SHUT_WR);
      close(connectionFD);
      break;
    }
  }
  co_return;
}

void ServerBuilder::SetPort(int port) { server_.port_ = port; }

void ServerBuilder::SetThreads(int numThreads) {
  server_.numThreads_ = numThreads;
}

void ServerBuilder::AddRequest(Method method, std::string_view path,
                               RespondType respond) {
  server_.trie_.AddRequest(method, respond, path);
}

Server ServerBuilder::Build() {
  if (server_.numThreads_ < 1) {
    server_.numThreads_ = 1;
  }
  return std::move(server_);
}

CoFuture<void> Server::Start() {
  std::signal(SIGPIPE, SIG_IGN);

  socketFD_ = socket(AF_INET, SOCK_STREAM, 0);
  if (socketFD_ == -1) {
    throw std::runtime_error("Could not open socket");
  }
  int reuse = 1;
  if (setsockopt(socketFD_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ==
      -1) {
    throw std::runtime_error("Could not set socket options");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port_);
  if (bind(socketFD_, (sockaddr *)&address, sizeof(address)) == -1) {
    throw std::runtime_error("Could not bind socket");
  }
  if (listen(socketFD_, SOMAXCONN) == -1) {
    throw std::runtime_error("Could not listen on socket");
  }
  for (int i = 0; i < numThreads_; ++i) {
    workerFutures_.push_back(RunCoroInThread([this] {
      IOUring ring;
      WorkerLoop(ring);
    }));
  }
  serverLoop_ = std::make_shared<CoPromise<void>>();
  return serverLoop_->GetFuture();
}

} // namespace HTTP
