#include "http_server.h"
#include "http_error.h"
#include "io_uring.h"
#include "read_iterator.h"
#include "request_data.h"
#include "trie.h"
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
using namespace HTTP;
Server::Server(Server &&rhs) {
  ring_ = std::move(rhs.ring_);
  trie_ = std::move(rhs.trie_);
  requestFutures_ = std::move(rhs.requestFutures_);
  socketFD_ = rhs.socketFD_;
  port_ = rhs.port_;
  stopFlag_.store(rhs.stopFlag_.load());
  main_ = std::move(rhs.main_);
  rhs.socketFD_ = -1;
}
Server::~Server() {
  stopFlag_ = true;
  if (socketFD_ != -1) {
    shutdown(socketFD_, SHUT_RDWR);
    close(socketFD_);
    socketFD_ = -1;
  }
  if (main_.valid()) {
    main_.wait();
  }
}
void Server::Accept() {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  socklen_t addr_len = sizeof(address);
  int connectionFD = ring_->Accept(socketFD_).get();
  if (connectionFD == -1) {
    if (stopFlag_.load()) {
      return;
    }
    throw std::runtime_error("Failed to open connection");
  }
  requestFutures_.push_back(std::async(
      std::launch::async, [this, connectionFD] { Process(connectionFD); }));
  while (!requestFutures_.empty() &&
         requestFutures_.front().wait_for(std::chrono::milliseconds(0)) ==
             std::future_status::ready) {
    requestFutures_.pop_front();
  }
}
void Server::WriteResponse(int connectionFD, const ResponseData &data) {
  std::stringstream text;
  text << "HTTP/1.1 " << data.status << ' '
       << (data.status / 100 == 2 ? "OK" : "ERROR") << "\r\n";
  auto headers = data.headers;
  if (!headers.contains("Content-Length")) {
    headers["Content-Length"] = std::to_string(data.body.size());
  }
  if (!headers.contains("Connection")) {
    headers["Connection"] = "close";
  }
  for (const auto &[name, value] : headers) {
    text << name << ": " << value << "\r\n";
  }
  text << "\r\n";
  text << data.body;
  std::string final = text.str();
  auto future = ring_->Write(connectionFD, final);
  future.get();
}
void Server::Process(int connectionFD) {
  ResponseData response;
  try {
    ReadIterator iterator(*ring_, connectionFD);
    RequestData request;
    request.method = iterator.ParseMethod();
    auto handler = GetHandler(request, iterator);
    iterator.ParseVariables(request);
    ++iterator;
    std::string protocol;
    while (*iterator && *iterator != '\n') {
      if (**iterator != '\r') {
        protocol += **iterator;
      }
      ++iterator;
    }
    if (!*iterator || protocol != "HTTP/1.1") {
      throw HTTPError(400, "Invalid request");
    }
    ++iterator;
    iterator.ParseHeaders(request);
    ++iterator;
    iterator.ParseBody(request);
    response = handler(request);
  } catch (HTTPError &error) {
    response.status = error.status;
    response.body = error.message;
  } catch (std::runtime_error &error) {
    response.status = 500;
    response.body = error.what();
  } catch (...) {
    response.status = 500;
    response.body = "What the actual fuck";
  }
  WriteResponse(connectionFD, response);
  close(connectionFD);
}
RespondType Server::GetHandler(RequestData &data, ReadIterator &iter) {
  if (*iter != ' ') {
    throw HTTPError(400, "Invalid request");
  }
  ++iter;
  if (*iter != '/') {
    throw HTTPError(400, "Invalid request");
  }
  auto current = &trie_.GetRoot();
  while (*iter != std::nullopt && *iter != ' ' && *iter != '?') {
    current = &current->Move(**iter);
    ++iter;
  }
  if (*iter == std::nullopt) {
    throw HTTPError(400, "Invalid request");
  }
  if (!current->handlers[data.method]) {
    throw HTTPError(404, "Not found");
  }
  return *current->handlers[data.method];
}
void ServerBuilder::SetPort(int port_) { server_.port_ = port_; }
void ServerBuilder::AddRequest(Method method, std::string_view path,
                               RespondType respond) {
  server_.trie_.AddRequest(method, respond, path);
}
Server ServerBuilder::Build() { return std::move(server_); }

void Server::Start() {
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
  main_ = std::async(std::launch::async, [this] {
    while (!stopFlag_.load()) {
      Accept();
    }
  });
}
