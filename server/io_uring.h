#pragma once
#include "iasio.h"
#include "mpsc_queue.h"
#include <array>
#include <atomic>
#include <future>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <optional>
#include <thread>
#define QUEUE_DEPTH 1024
namespace HTTP {
class IOUring : IAsio {
private:
  struct Entry {
    enum { ACCEPT, READ, WRITE } type;
    int fd;
    std::optional<const char *> toWrite;
    std::optional<char *> toRead;
    std::optional<size_t> len;
  };
  MPSCQueue<Entry> queue_;
  io_uring ring_;
  std::array<std::optional<std::promise<int>>, 1025> fdToPromise_;
  std::atomic<bool> stopToken_ = false;
  std::atomic_uint64_t inProcess_ = 0;
  std::thread workerThread_;
  void ProcessCalls();
  void AddEntries();
  void SubmitEntry(int *value);

public:
  ~IOUring();
  IOUring();
  IOUring &operator=(IOUring &&rhs);
  std::future<int> Read(int fileDescriptor, std::array<char, 256> &buffer);
  std::future<int> Write(int fileDescriptor, const std::string &data);
  std::future<int> Accept(int fileDescriptor);
};
} // namespace HTTP
