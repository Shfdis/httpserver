#pragma once
#include "co_future.h"
#include "http_parser.h"
#include "io_uring.h"
#include "non_owning_co_future.h"
#include "request_data.h"
#include "trie.h"
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace HTTP {
class Server {
private:
  int socketFD_{-1};
  int port_{0};
  int numThreads_{1};
  Trie trie_;
  std::vector<CoFuture<void>> workerFutures_;
  std::atomic_bool stopFlag_{false};
  std::shared_ptr<CoPromise<void>> serverLoop_;
  
  void WorkerLoop(IOUring &ring);
  NonOwningCoFuture<void>
  AcceptAndProcess(IOUring &ring,
                   std::vector<NonOwningCoFuture<void>> &connections);
  NonOwningCoFuture<void> WriteRaw(IOUring &ring, int connectionFD,
                                   std::string_view data);
  NonOwningCoFuture<void> WriteResponse(IOUring &ring, int connectionFD,
                                        const ResponseData &data,
                                        const RequestData &request,
                                        bool keepAlive, std::string &buffer);
  NonOwningCoFuture<void> Process(IOUring &ring, int connectionFD);
  friend class ServerBuilder;

public:
  Server() = default;
  ~Server();
  Server(Server &&rhs);
  CoFuture<void> Start();
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
}
