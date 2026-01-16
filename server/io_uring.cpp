#include "io_uring.h"
#include <cstdint>
#include <liburing/io_uring.h>
#include <optional>
#include <stdexcept>
#include <thread>
namespace HTTP {
IOUring::~IOUring() {
  stopToken_ = true;
  wakeCv_.notify_all();
  if (workerThread_.joinable()) {
    workerThread_.join();
  }
  io_uring_queue_exit(&ring_);
}

void IOUring::NotifyWorker() {
  wakeCv_.notify_one();
}

IOUring::IOUring() {
  if (io_uring_queue_init(QUEUE_DEPTH, &ring_, IORING_SETUP_SQPOLL) < 0) {
    throw std::runtime_error("Failed to initilize io_uring");
  }
  workerThread_ = std::thread([this] {
    while (!stopToken_) {
      {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        wakeCv_.wait_for(lock, std::chrono::milliseconds(1), [this] {
          return stopToken_ || !queue_.Empty() || inProcess_ > 0;
        });
      }
      if (stopToken_) break;
      
      AddEntries();
      uint64_t inProcess = inProcess_;
      for (int count = 0; count < QUEUE_DEPTH && count < inProcess; ++count) {
        ProcessCalls();
      }
    }
  });
}
void IOUring::AddEntries() {
  size_t fdsSize = 0;
  for (int count = 0; count < QUEUE_DEPTH && !queue_.Empty(); count++) {
    auto entry = queue_.Consume();
    auto sqEntry = io_uring_get_sqe(&ring_);
    if (sqEntry == nullptr) {
      throw std::runtime_error("Could not create an entry");
    }
    int *value = new int(entry.fd);
    if (entry.type == Entry::READ) [[likely]] {
      io_uring_prep_read(sqEntry, entry.fd, entry.toRead.value(), 256, 0);
    } else if (entry.type == Entry::ACCEPT) {
      io_uring_prep_accept(sqEntry, entry.fd, nullptr, nullptr, 0);
    } else {
      io_uring_prep_write(sqEntry, entry.fd, entry.toWrite.value(), *entry.len,
                          0);
    }
    io_uring_sqe_set_data(sqEntry, value);
    fds_[fdsSize++] = value;
  }
  if (fdsSize > 0) {
    if (io_uring_submit(&ring_) < 0) {
      for (int i = 0; i < fdsSize; ++i) {
        delete fds_[i];
      }
      throw std::runtime_error("Could not submit values");
    }
    inProcess_ += fdsSize;
  }
}
std::future<int> IOUring::Write(int fileDescriptor, const std::string &data) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  std::promise<int> promise;
  auto future = promise.get_future();
  fdToPromise_[fileDescriptor] = std::move(promise);
  queue_.Push(
      {Entry::WRITE, fileDescriptor, data.c_str(), std::nullopt, data.size()});
  NotifyWorker();
  return future;
}
std::future<int> IOUring::Read(int fileDescriptor,
                               std::array<char, 256> &buffer) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  std::promise<int> promise;
  auto future = promise.get_future();
  fdToPromise_[fileDescriptor] = std::move(promise);
  queue_.Push({Entry::READ, fileDescriptor, std::nullopt, buffer.begin()});
  NotifyWorker();
  return future;
}
std::future<int> IOUring::Accept(int fileDescriptor) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  std::promise<int> promise;
  auto future = promise.get_future();
  fdToPromise_[fileDescriptor] = std::move(promise);
  queue_.Push({Entry::ACCEPT, fileDescriptor, std::nullopt, std::nullopt});
  NotifyWorker();
  return future;
}
void IOUring::ProcessCalls() {
  io_uring_cqe *cqEntry;
  struct __kernel_timespec ts = {.tv_sec = 0,
                                 .tv_nsec = 1000000}; // 1ms timeout
  int ret = io_uring_wait_cqe_timeout(&ring_, &cqEntry, &ts);
  if (ret == -ETIME) {
    return; 
  }
  if (ret < 0) {
    throw std::runtime_error("Could not get entry from completion queue");
  }
  inProcess_--;
  int *data = (int *)io_uring_cqe_get_data(cqEntry);
  int fd = *data;
  delete data;
  if (!fdToPromise_[fd]) {
    io_uring_cqe_seen(&ring_, cqEntry);
    return;
  }
  if (cqEntry->res < 0) {
    std::runtime_error err("Could not perform call");
    fdToPromise_[fd].value().set_exception(std::make_exception_ptr(err));
    io_uring_cqe_seen(&ring_, cqEntry);
    return;
  }
  fdToPromise_[fd].value().set_value(cqEntry->res);
  io_uring_cqe_seen(&ring_, cqEntry);
}
} // namespace HTTP
