#include "server.h"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

int PickPort() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error("socket failed");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
    close(fd);
    throw std::runtime_error("bind failed");
  }
  socklen_t length = sizeof(address);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) < 0) {
    close(fd);
    throw std::runtime_error("getsockname failed");
  }
  int port = ntohs(address.sin_port);
  close(fd);
  return port;
}

int Connect(int port) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      throw std::runtime_error("socket failed");
    }

    timeval timeout{};
    timeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
        0) {
      return fd;
    }
    close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("connect failed");
}

void SendAll(int fd, std::string_view data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t result = send(fd, data.data() + sent, data.size() - sent, 0);
    if (result <= 0) {
      throw std::runtime_error("send failed");
    }
    sent += static_cast<size_t>(result);
  }
}

std::string ReadUntilClose(int fd) {
  std::string data;
  char buffer[2048];
  while (true) {
    ssize_t result = recv(fd, buffer, sizeof(buffer), 0);
    if (result > 0) {
      data.append(buffer, static_cast<size_t>(result));
      continue;
    }
    if (result == 0) {
      return data;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      throw std::runtime_error("recv timed out");
    }
    throw std::runtime_error("recv failed");
  }
}

std::string ReadUntilContains(int fd, std::string_view needle) {
  std::string data;
  char buffer[2048];
  while (data.find(needle) == std::string::npos) {
    ssize_t result = recv(fd, buffer, sizeof(buffer), 0);
    if (result > 0) {
      data.append(buffer, static_cast<size_t>(result));
      continue;
    }
    if (result == 0) {
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      throw std::runtime_error("recv timed out");
    }
    throw std::runtime_error("recv failed");
  }
  return data;
}

std::string RequestOnce(int port, std::string_view request) {
  int fd = Connect(port);
  SendAll(fd, request);
  std::string response = ReadUntilClose(fd);
  close(fd);
  return response;
}

size_t Count(std::string_view haystack, std::string_view needle) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}
} // namespace

int main() {
  try {
    const int port = PickPort();
    HTTP::ServerBuilder builder;
    builder.SetPort(port);
    builder.SetThreads(1);
    builder.AddRequest(HTTP::GET, "/echo",
                       [](const HTTP::RequestData &request) {
                         HTTP::ResponseData response;
                         response.status = 200;
                         auto it = request.params.find("msg");
                         if (it != request.params.end()) {
                           response.body = it->second;
                         }
                         return response;
                       });
    builder.AddRequest(HTTP::POST, "/echo",
                       [](const HTTP::RequestData &request) {
                         HTTP::ResponseData response;
                         response.status = 200;
                         response.body = request.body;
                         if (auto trailer = request.trailers.find("Trace");
                             trailer != request.trailers.end()) {
                           response.headers["X-Trailer"] = trailer->second;
                         }
                         return response;
                       });

    auto server = builder.Build();
    auto serverFuture = server.Start();

    {
      std::string response =
          RequestOnce(port, "GET /echo?msg=hello HTTP/1.1\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 200 OK") != std::string::npos,
            "GET returns 200");
      Check(response.ends_with("hello"), "GET response body is returned");
    }

    {
      std::string response = RequestOnce(
          port,
          "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
          "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
          "5\r\nhello\r\n0\r\nTrace: yes\r\n\r\n");
      Check(response.find("HTTP/1.1 200 OK") != std::string::npos,
            "chunked POST returns 200");
      Check(response.find("X-Trailer: yes") != std::string::npos,
            "chunked trailer reaches handler");
      Check(response.ends_with("hello"), "chunked body is decoded");
    }

    {
      int fd = Connect(port);
      SendAll(fd, "GET /echo?msg=one HTTP/1.1\r\nHost: localhost\r\n\r\n"
                  "GET /echo?msg=two HTTP/1.1\r\nHost: localhost\r\n"
                  "Connection: close\r\n\r\n");
      std::string response = ReadUntilClose(fd);
      close(fd);
      Check(Count(response, "HTTP/1.1 200 OK") == 2,
            "pipelined requests receive two responses");
      Check(response.find("one") != std::string::npos &&
                response.find("two") != std::string::npos,
            "pipelined response bodies are present");
    }

    {
      std::string response =
          RequestOnce(port, "HEAD /echo?msg=head HTTP/1.1\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 200 OK") != std::string::npos,
            "HEAD falls back to GET handler");
      Check(response.find("Content-Length: 4") != std::string::npos,
            "HEAD preserves representation length");
      const size_t body = response.find("\r\n\r\n");
      Check(body != std::string::npos && body + 4 == response.size(),
            "HEAD response has no body");
    }

    {
      std::string response =
          RequestOnce(port, "OPTIONS /echo HTTP/1.1\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 200 OK") != std::string::npos,
            "OPTIONS is automatic");
      Check(response.find("Allow: GET, HEAD, OPTIONS, POST") !=
                std::string::npos,
            "OPTIONS includes Allow");
    }

    {
      std::string response =
          RequestOnce(port, "OPTIONS * HTTP/1.1\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 200 OK") != std::string::npos,
            "OPTIONS * is automatic");
      Check(response.find("Allow: GET, HEAD, OPTIONS, POST") !=
                std::string::npos,
            "OPTIONS * includes server-wide Allow");
    }

    {
      std::string response =
          RequestOnce(port, "PUT /echo HTTP/1.1\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 405 Method Not Allowed") !=
                std::string::npos,
            "unsupported method on existing path returns 405");
      Check(response.find("Allow: GET, HEAD, OPTIONS, POST") !=
                std::string::npos,
            "405 includes Allow");
    }

    {
      std::string response =
          RequestOnce(port, "FOO /echo HTTP/1.1\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 501 Not Implemented") != std::string::npos,
            "unknown valid method returns 501");
    }

    {
      std::string response =
          RequestOnce(port, "GET /missing HTTP/1.1\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 404 Not Found") != std::string::npos,
            "missing path returns 404");
    }

    {
      std::string response =
          RequestOnce(port, "GET / HTTP/2.0\r\nHost: localhost\r\n"
                            "Connection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 505 HTTP Version Not Supported") !=
                std::string::npos,
            "unsupported version returns 505");
    }

    {
      std::string response =
          RequestOnce(port, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
      Check(response.find("HTTP/1.1 400 Bad Request") != std::string::npos,
            "missing Host returns 400");
    }

    {
      int fd = Connect(port);
      SendAll(fd, "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                  "Expect: 100-continue\r\nContent-Length: 5\r\n"
                  "Connection: close\r\n\r\n");
      std::string interim = ReadUntilContains(fd, "HTTP/1.1 100 Continue");
      Check(interim.find("HTTP/1.1 100 Continue") != std::string::npos,
            "100 Continue is sent before body");
      SendAll(fd, "hello");
      std::string final = ReadUntilClose(fd);
      close(fd);
      Check(final.find("HTTP/1.1 200 OK") != std::string::npos,
            "continued request gets final 200");
      Check(final.ends_with("hello"), "continued body reaches handler");
    }
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
