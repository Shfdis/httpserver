#pragma once
#include "iasio.h"
#include "read_iterator.h"
#include "request_data.h"
#include "trie.h"
#include <deque>
#include <future>
namespace HTTP {
class Server {
private:
  std::deque<std::future<void>> requestFutures_;
  IAsioPtr ring_;
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
  void SetAsio(IAsioPtr asio);
  void SetPort(int port);
  void AddRequest(Method method, std::string_view path, RespondType respond);
  Server Build();
};
} // namespace HTTP
