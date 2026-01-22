#pragma once
#include "coroutine.h"
#include <array>
#include <atomic>
#include <coroutine>
#include <deque>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <optional>
#include <string>
#define QUEUE_DEPTH 1024
namespace HTTP {
struct Promise;
class IOUring;

struct ReadAwaiter {
  IOUring &ring_;
  int fd_;
  std::array<char, 256> &buffer_;
  std::coroutine_handle<Promise> coro_{};
  
  ReadAwaiter(IOUring &ring, int fd, std::array<char, 256> &buffer)
    : ring_(ring), fd_(fd), buffer_(buffer) {}
  
  bool await_ready() const noexcept { return false; }
  
  void await_suspend(std::coroutine_handle<> h);
  
  size_t await_resume() {
    size_t result = coro_.promise().readResult_;
    return result;
  }
};

struct AcceptAwaiter {
  IOUring &ring_;
  int fd_;
  std::coroutine_handle<Promise> coro_;
  
  AcceptAwaiter(IOUring &ring, int fd)
    : ring_(ring), fd_(fd) {}
  
  bool await_ready() const noexcept { return false; }
  
  void await_suspend(std::coroutine_handle<> h);
  
  int await_resume() {
    int result = coro_ ? coro_.promise().acceptResult_ : -1;
    return result;
  }
};

struct WriteAwaiter {
  IOUring &ring_;
  int fd_;
  std::shared_ptr<std::string> data_;
  size_t offset_{0};
  size_t len_{0};
  std::coroutine_handle<Promise> coro_{};
  
  WriteAwaiter(IOUring &ring, int fd, std::shared_ptr<std::string> data, size_t offset,
               size_t len)
      : ring_(ring), fd_(fd), data_(std::move(data)), offset_(offset), len_(len) {}
  
  bool await_ready() const noexcept { return false; }
  
  void await_suspend(std::coroutine_handle<> h);
  
  size_t await_resume() const noexcept {
    return coro_.promise().writeResult_;
  }
};

class IOUring {
  friend struct ReadAwaiter;
  friend struct AcceptAwaiter;
  friend struct WriteAwaiter;
public:
  enum OpType { ACCEPT, READ, WRITE };
private:
  struct Entry {
    OpType type;
    int fd;
    std::optional<char *> toRead;
    std::coroutine_handle<> coro;
    std::shared_ptr<std::string> writeData;
    size_t writeOffset{0};
    size_t writeLen{0};
  };
  struct SqeData {
    int fd;
    OpType opType;
    std::coroutine_handle<> coro;
    std::shared_ptr<std::string> writeData;
    size_t writeOffset{0};
    size_t writeLen{0};
  };
  std::deque<Entry> queue_;
  io_uring ring_;
  std::array<std::optional<int>, 1025> fdToAcceptResult_;
  std::atomic<bool> stopToken_ = false;
  std::atomic_uint64_t inProcess_ = 0;
  void ProcessCalls();
  void AddEntries();

public:
  void Poll();
  ~IOUring();
  IOUring();
  IOUring &operator=(IOUring &&rhs);
  void Read(int fileDescriptor, std::array<char, 256> &buffer, std::coroutine_handle<> coro);
  ReadAwaiter ReadAsync(int fileDescriptor, std::array<char, 256> &buffer);
  void Write(int fileDescriptor, std::shared_ptr<std::string> data, size_t offset, size_t len,
             std::coroutine_handle<> coro);
  WriteAwaiter WriteAsync(int fileDescriptor, std::shared_ptr<std::string> data, size_t offset,
                          size_t len);
  void Accept(int fileDescriptor, std::coroutine_handle<> coro);
  AcceptAwaiter AcceptAsync(int fileDescriptor);
  int GetAcceptResult(int fileDescriptor);
};
}
