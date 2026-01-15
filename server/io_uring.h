#pragma once
#include <array>
#include <atomic>
#include <future>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#define QUEUE_DEPTH 64
namespace HTTP {
class IOUring {
private:
  std::mutex submitMut;
  std::mutex promiseMut;
  io_uring ring_;
  std::unordered_map<int, std::promise<int>> fdToPromise_;
  std::atomic<bool> stopToken_ = false;
  std::thread readerThread_;
  void ProcessCalls();
  void SubmitEntry(int *value);

public:
  ~IOUring();
  IOUring();
  IOUring &operator=(IOUring &&rhs);
  std::future<int> Read(int fileDescriptor, std::array<char, 256> &buffer);
  std::future<int> Write(int fileDescriptor, const std::string &data);
};
} // namespace HTTP
