#pragma once
#include "io_uring.h"
#include "read_iterator.h"
#include "request_data.h"
#include "trie.h"
#include <deque>
#include <future>
#include <memory>
namespace HTTP {
class Server {
private:
  std::deque<std::future<void>> requestFutures_;
  std::unique_ptr<IOUring> ring_ = std::make_unique<IOUring>();
  int socketFD_{-1};
  int port_{0};
  Trie trie_;
  void Accept();
  RespondType GetHandler(RequestData &data, ReadIterator &iter);
  void WriteResponse(int connectionFD, const ResponseData &data);
  void Process(int connectionFD);
  std::future<void> main_;
  std::atomic_bool stopFlag_{false};
  friend class ServerBuilder;

public:
  Server() = default;
  ~Server();
  Server(Server &&rhs);
  void Start();
};
class ServerBuilder {
private:
  Server server_;
  int port_;

public:
  void SetPort(int port);
  void AddRequest(Method method, std::string_view path, RespondType respond);
  Server Build();
};
} // namespace HTTP
