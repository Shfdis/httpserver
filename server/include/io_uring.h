#pragma once
#include "co_future.h"
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <optional>
#include <string>
#define QUEUE_DEPTH 1024
namespace HTTP {
class IOUring;

class IOUring {
public:
  enum OpType { ACCEPT, READ, WRITE };
private:
  struct Entry {
    OpType type;
    int fd;
    std::optional<char *> toRead;
    std::function<void(int)> complete;
    std::shared_ptr<std::string> writeData;
    size_t writeOffset{0};
    size_t writeLen{0};
  };
  struct SqeData {
    std::function<void(int)> complete;
    std::shared_ptr<std::string> writeData;
    size_t writeOffset{0};
    size_t writeLen{0};
  };
  std::deque<Entry> queue_;
  io_uring ring_;
  std::atomic_uint64_t inProcess_ = 0;
  void ProcessCalls();
  void AddEntries();

public:
  void Poll();
  ~IOUring();
  IOUring();
  IOUring &operator=(IOUring &&rhs);
  void Read(int fileDescriptor, std::array<char, 256> &buffer, std::function<void(int)> complete);
  CoFuture<size_t> ReadAsync(int fileDescriptor, std::array<char, 256> &buffer);
  void Write(int fileDescriptor, std::shared_ptr<std::string> data, size_t offset, size_t len,
             std::function<void(int)> complete);
  CoFuture<size_t> WriteAsync(int fileDescriptor, std::shared_ptr<std::string> data, size_t offset,
                              size_t len);
  void Accept(int fileDescriptor, std::function<void(int)> complete);
  CoFuture<int> AcceptAsync(int fileDescriptor);
};
}
