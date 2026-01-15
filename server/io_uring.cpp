#include "io_uring.h"
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace HTTP {
IOUring::~IOUring() {
  stopToken_ = true;
  if (readerThread_.joinable()) {
    readerThread_.join();
  }
  io_uring_queue_exit(&ring_);
}

IOUring::IOUring() {
  if (io_uring_queue_init(QUEUE_DEPTH, &ring_, IORING_SETUP_SQPOLL) < 0) {
    throw std::runtime_error("Failed to initilize io_uring");
  }
  readerThread_ = std::thread([this] {
    while (!stopToken_) {
      ProcessCalls();
    }
  });
}
std::future<int> IOUring::Write(int fileDescriptor, const std::string &data) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  std::promise<int> promise;
  auto future = promise.get_future();
  {
    std::lock_guard lock(promiseMut);
    fdToPromise_[fileDescriptor] = std::move(promise);
  }
  int *value = nullptr;
  {
    std::lock_guard lock(submitMut);
    auto sqEntry = io_uring_get_sqe(&ring_);
    if (!sqEntry) {
      throw std::runtime_error("Could not create an entry");
    }
    sqEntry->len = data.size();
    sqEntry->fd = fileDescriptor;
    sqEntry->opcode = IORING_OP_WRITE;
    sqEntry->addr = reinterpret_cast<__u64>(data.c_str());
    value = new int(fileDescriptor);
    io_uring_sqe_set_data(sqEntry, value);
  }
  SubmitEntry(value);
  return future;
}
std::future<int> IOUring::Read(int fileDescriptor,
                               std::array<char, 256> &buffer) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  std::promise<int> promise;
  auto future = promise.get_future();
  {
    std::lock_guard lock(promiseMut);
    fdToPromise_[fileDescriptor] = std::move(promise);
  }
  int *value = nullptr;
  {
    std::lock_guard lock(submitMut);
    auto sqEntry = io_uring_get_sqe(&ring_);
    if (sqEntry == nullptr) {
      throw std::runtime_error("Could not create an entry");
    }
    sqEntry->len = 256;
    sqEntry->fd = fileDescriptor;
    sqEntry->opcode = IORING_OP_READ;
    sqEntry->addr = reinterpret_cast<__u64>(buffer.data());
    value = new int(fileDescriptor);
    io_uring_sqe_set_data(sqEntry, value);
  }
  SubmitEntry(value);
  return future;
}
void IOUring::ProcessCalls() {
  io_uring_cqe *cqEntry;
  if (io_uring_cq_ready(&ring_) == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return;
  }
  if (io_uring_wait_cqe(&ring_, &cqEntry) < 0) {
    throw std::runtime_error("Could not get entry from completion queue");
  }
  int *data = (int *)io_uring_cqe_get_data(cqEntry);
  int fd = *data;
  delete data;
  std::promise<int> promise;
  {
    std::lock_guard lock(promiseMut);
    auto it = fdToPromise_.find(fd);
    if (it == fdToPromise_.end()) {
      io_uring_cqe_seen(&ring_, cqEntry);
      return;
    }
    promise = std::move(it->second);
    fdToPromise_.erase(it);
  }
  if (cqEntry->res < 0) {
    std::runtime_error err("Could not perform call");
    promise.set_exception(std::make_exception_ptr(err));
    io_uring_cqe_seen(&ring_, cqEntry);
    return;
  }
  promise.set_value(cqEntry->res);
  io_uring_cqe_seen(&ring_, cqEntry);
}
void IOUring::SubmitEntry(int *value) {
  std::lock_guard lock(submitMut);
  if (io_uring_submit(&ring_) < 0) {
    delete value;
    throw std::runtime_error("Could not submit value");
  }
}
IOUring &IOUring::operator=(IOUring &&rhs) {
  if (this == &rhs) {
    return *this;
  }
  stopToken_ = true;
  if (readerThread_.joinable()) {
    readerThread_.join();
  }
  io_uring_queue_exit(&ring_);
  {
    std::lock_guard lock(promiseMut);
    fdToPromise_ = std::move(rhs.fdToPromise_);
  }
  ring_ = std::move(rhs.ring_);
  stopToken_ = rhs.stopToken_.load();
  readerThread_ = std::move(rhs.readerThread_);
  return *this;
}
} // namespace HTTP
