#pragma once
#include "co_future.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <string>
#include <string_view>
#define QUEUE_DEPTH 1024
namespace HTTP {
inline constexpr size_t kReadBufferSize = 256;

class IOUring;

class IOUring {
public:
  enum OpType { ACCEPT, READ, WRITE };
private:
  struct Entry {
    OpType type;
    int fd;
    char *toRead{nullptr};
    std::shared_ptr<CoFuture<int>::ControlBlock> control;
    std::string_view writeData;
    size_t writeOffset{0};
    size_t writeLen{0};
  };
  struct SqeData {
    std::shared_ptr<CoFuture<int>::ControlBlock> control;
    std::string_view writeData;
    size_t writeOffset{0};
    size_t writeLen{0};
    bool inUse{false};
  };
  std::deque<Entry> queue_;
  std::array<SqeData, QUEUE_DEPTH> sqeData_{};
  io_uring ring_;
  std::atomic_uint64_t inProcess_ = 0;
  size_t nextSqeData_{0};
  SqeData *AcquireSqeData();
  void ProcessCalls();
  void AddEntries();

public:
  void Poll();
  ~IOUring();
  IOUring();
  IOUring &operator=(IOUring &&rhs);
  void Read(int fileDescriptor, std::array<char, kReadBufferSize> &buffer,
            std::shared_ptr<CoFuture<int>::ControlBlock> control);
  CoFuture<int> ReadAsync(int fileDescriptor, std::array<char, kReadBufferSize> &buffer);
  void Write(int fileDescriptor, std::string_view data, size_t offset, size_t len,
             std::shared_ptr<CoFuture<int>::ControlBlock> control);
  CoFuture<int> WriteAsync(int fileDescriptor, std::string_view data, size_t offset,
                              size_t len);
  void Accept(int fileDescriptor,
              std::shared_ptr<CoFuture<int>::ControlBlock> control);
  CoFuture<int> AcceptAsync(int fileDescriptor);
};
}
