#pragma once
#include "io_uring.h"
#include "read_iterator.h"
#include "request_data.h"
#include "trie.h"
#include <thread>
#include <vector>
namespace HTTP {
class Server {
private:
  int socketFD_{-1};
  int port_{0};
  int numThreads_{1};
  Trie trie_;
  std::vector<std::thread> workerThreads_;
  std::atomic_bool stopFlag_{false};
  
  void WorkerLoop();
  RespondType GetHandler(RequestData &data, ReadIterator &iter);
  void WriteResponse(IOUring &ring, int connectionFD, const ResponseData &data);
  void Process(IOUring &ring, int connectionFD);
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

public:
  void SetThreads(int numThreads);
  void SetPort(int port);
  void AddRequest(Method method, std::string_view path, RespondType respond);
  Server Build();
};
} // namespace HTTP
