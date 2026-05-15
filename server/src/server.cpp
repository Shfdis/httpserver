#include "server.h"
#include "coroutine.h"
#include "http_error.h"
#include "read_iterator.h"
#include "request_data.h"
#include "trie.h"
#include <algorithm>
#include <cctype>
#include <csignal>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace HTTP {

namespace {
static bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

static std::string trim_copy(std::string_view s) {
  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return std::string(s.substr(start, end - start));
}

static std::optional<std::string_view>
find_header_ci(const std::unordered_map<std::string, std::string> &headers,
               std::string_view key) {
  for (const auto &[k, v] : headers) {
    if (iequals(k, key))
      return v;
  }
  return std::nullopt;
}

static bool wants_close(const RequestData &request) {
  auto v = find_header_ci(request.headers, "Connection");
  if (!v)
    return false;
  std::string value = trim_copy(*v);
  for (auto &ch : value)
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return value.find("close") != std::string::npos;
}
} // namespace

Server::Server(Server &&rhs) {
  trie_ = std::move(rhs.trie_);
  socketFD_ = rhs.socketFD_;
  port_ = rhs.port_;
  numThreads_ = rhs.numThreads_;
  stopFlag_.store(rhs.stopFlag_.load());
  pendingAccepts_.store(rhs.pendingAccepts_.load());
  workerThreads_ = std::move(rhs.workerThreads_);
  rhs.socketFD_ = -1;
}

Server::~Server() {
  stopFlag_ = true;
  if (socketFD_ != -1) {
    shutdown(socketFD_, SHUT_RDWR);
    close(socketFD_);
    socketFD_ = -1;
  }
  for (auto &t : workerThreads_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

Coroutine Server::AcceptAndProcess(IOUring &ring) {
  static thread_local std::vector<Coroutine> processCoros;

  while (!stopFlag_.load()) {
    processCoros.erase(
        std::remove_if(processCoros.begin(), processCoros.end(),
                       [](const Coroutine &c) { return !c || c.done(); }),
        processCoros.end());

    int connectionFD = co_await ring.AcceptAsync(socketFD_);

    if (connectionFD < 0) {
      if (stopFlag_.load()) {
        co_return;
      }
      continue;
    }

    Coroutine proc = Process(ring, connectionFD);
    proc.resume();
    processCoros.push_back(std::move(proc));
  }
  co_return;
}

void Server::WorkerLoop(IOUring &ring) {
  try {
    Coroutine acceptCoro = AcceptAndProcess(ring);
    acceptCoro.resume();

    while (!stopFlag_.load()) {
      ring.Poll();

      if (acceptCoro.done()) {
        acceptCoro = AcceptAndProcess(ring);
        acceptCoro.resume();
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "[WorkerLoop] Exception: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[WorkerLoop] Unknown exception" << std::endl;
  }
}

Coroutine Server::WriteResponse(IOUring &ring, int connectionFD,
                                const ResponseData &data, bool keepAlive) {
  std::stringstream text;
  text << "HTTP/1.1 " << data.status << ' '
       << (data.status / 100 == 2 ? "OK" : "ERROR") << "\r\n";
  auto headers = data.headers;
  if (!headers.contains("Content-Length")) {
    headers["Content-Length"] = std::to_string(data.body.size());
  }
  headers["Connection"] = keepAlive ? "keep-alive" : "close";
  for (const auto &[name, value] : headers) {
    text << name << ": " << value << "\r\n";
  }
  text << "\r\n";
  text << data.body;
  auto final = std::make_shared<std::string>(text.str());
  size_t sent = 0;
  while (sent < final->size()) {
    size_t wrote = co_await ring.WriteAsync(connectionFD, final, sent,
                                            final->size() - sent);
    if (wrote == 0) {
      break;
    }
    sent += wrote;
  }
  co_return;
}

Coroutine Server::Process(IOUring &ring, int connectionFD) {
  ReadIterator iterator(ring, connectionFD);

  while (true) {
    ResponseData response;
    bool keepAlive = true;
    bool mustClose = false;

    try {
      RequestData request;
      co_await iterator.Ensure();
      if (!iterator) {
        mustClose = true;
        keepAlive = false;
        throw HTTPError(400, "");
      }
      co_await iterator.ParseMethod(request);
      RespondType handler;
      co_await GetHandler(request, iterator, handler);
      co_await iterator.ParseVariables(request);
      co_await ++iterator;
      std::string protocol;
      while (true) {
        co_await iterator.Ensure();
        if (!iterator) {
          throw HTTPError(400, "Invalid request");
        }
        char c = *iterator;
        if (c == '\n') {
          break;
        }
        if (c != '\r') {
          protocol += c;
        }
        co_await ++iterator;
      }
      if (protocol != "HTTP/1.1") {
        throw HTTPError(400, "Invalid request");
      }
      co_await ++iterator;
      co_await iterator.ParseHeaders(request);
      co_await iterator.ParseBody(request);
      keepAlive = !wants_close(request);
      response = handler(request);
    } catch (HTTPError &error) {
      response.status = error.status;
      response.body = error.message;
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
      co_await WriteResponse(ring, connectionFD, response, keepAlive);
    }
    if (!keepAlive || mustClose) {
      (void)shutdown(connectionFD, SHUT_WR);
      close(connectionFD);
      break;
    }
  }
  co_return;
}

Coroutine Server::GetHandler(RequestData &data, ReadIterator &iter,
                             RespondType &handler) {
  co_await iter.Ensure();
  if (*iter != ' ') {
    throw HTTPError(400, "Invalid request");
  }
  co_await ++iter;
  co_await iter.Ensure();
  if (*iter != '/') {
    throw HTTPError(400, "Invalid request");
  }
  auto current = &trie_.GetRoot();
  bool inVariable = false;
  while (true) {
    co_await iter.Ensure();
    if (!iter) {
      throw HTTPError(400, "Invalid request");
    }
    char c = *iter;
    if (c == ' ' || c == '?') {
      break;
    }
    if (!current->children.contains(c) && current->any) {
      if (!inVariable) {
        inVariable = true;
        data.urlVariables.push_back("");
      }
      data.urlVariables.back().push_back(c);
      co_await ++iter;
      continue;
    }
    inVariable = false;
    current = &current->Move(c);
    co_await ++iter;
  }
  if (!current->handlers[data.method]) {
    throw HTTPError(404, "Not found");
  }
  handler = *current->handlers[data.method];
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

void Server::Start() {
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
    workerThreads_.emplace_back([this] {
      IOUring ring;
      WorkerLoop(ring);
    });
  }
}

} // namespace HTTP
