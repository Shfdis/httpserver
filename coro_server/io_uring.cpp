#include "io_uring.h"
#include "coroutine.h"
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <liburing/io_uring.h>
#include <optional>
#include <stdexcept>
#include <linux/errno.h>

namespace HTTP {

IOUring::~IOUring() {
  stopToken_ = true;
  io_uring_queue_exit(&ring_);
}

IOUring::IOUring() {
  int ret = io_uring_queue_init(QUEUE_DEPTH, &ring_, 0);
  if (ret < 0) {
    throw std::runtime_error("Failed to initialize io_uring");
  }
}

void IOUring::AddEntries() {
  size_t fdsSize = 0;
  for (int count = 0; count < QUEUE_DEPTH && !queue_.empty(); count++) {
    auto entry = std::move(queue_.front());
    queue_.pop_front();
    
    auto sqEntry = io_uring_get_sqe(&ring_);
    if (sqEntry == nullptr) {
      queue_.push_front(std::move(entry));
      break;
    }
    
    SqeData *sqeData = new SqeData{
        entry.fd, entry.type, entry.coro, std::move(entry.writeData), entry.writeOffset,
        entry.writeLen};
    
    if (entry.type == IOUring::READ) [[likely]] {
      io_uring_prep_read(sqEntry, entry.fd, entry.toRead.value(), 256, 0);
    } else if (entry.type == IOUring::ACCEPT) {
      io_uring_prep_accept(sqEntry, entry.fd, nullptr, nullptr, 0);
    } else {
      const char *ptr = sqeData->writeData->data() + sqeData->writeOffset;
      io_uring_prep_write(sqEntry, entry.fd, ptr, sqeData->writeLen, 0);
    }
    io_uring_sqe_set_data(sqEntry, sqeData);
    fdsSize++;
  }
  if (fdsSize > 0) {
    int submitResult = io_uring_submit(&ring_);
    if (submitResult >= 0) {
      inProcess_ += fdsSize;
    }
  }
}

void IOUring::Poll() {
  try {
    AddEntries();
    
    if (inProcess_.load() > 0) {
      ProcessCalls();
    }
    
    if (!queue_.empty()) {
      AddEntries();
    }
    
    io_uring_submit(&ring_);
    
  } catch (const std::exception &e) {
    std::cerr << "[Poll] Exception: " << e.what() << std::endl;
    throw;
  }
}

void IOUring::Write(int fileDescriptor, std::shared_ptr<std::string> data, size_t offset,
                    size_t len, std::coroutine_handle<> coro) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  Entry entry;
  entry.type = IOUring::WRITE;
  entry.fd = fileDescriptor;
  entry.writeData = std::move(data);
  entry.writeOffset = offset;
  entry.writeLen = len;
  entry.coro = coro;
  queue_.push_back(std::move(entry));
  AddEntries();
}

void IOUring::Read(int fileDescriptor, std::array<char, 256> &buffer, std::coroutine_handle<> coro) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  Entry entry;
  entry.type = IOUring::READ;
  entry.fd = fileDescriptor;
  entry.toRead = buffer.begin();
  entry.coro = coro;
  queue_.push_back(entry);
}

ReadAwaiter IOUring::ReadAsync(int fileDescriptor, std::array<char, 256> &buffer) {
  return ReadAwaiter(*this, fileDescriptor, buffer);
}

void ReadAwaiter::await_suspend(std::coroutine_handle<> h) {
  coro_ = std::coroutine_handle<Promise>::from_address(h.address());
  ring_.Read(fd_, buffer_, h);
}

void AcceptAwaiter::await_suspend(std::coroutine_handle<> h) {
  coro_ = std::coroutine_handle<Promise>::from_address(h.address());
  ring_.Accept(fd_, h);
}

void WriteAwaiter::await_suspend(std::coroutine_handle<> h) {
  coro_ = std::coroutine_handle<Promise>::from_address(h.address());
  ring_.Write(fd_, std::move(data_), offset_, len_, h);
}

void IOUring::Accept(int fileDescriptor, std::coroutine_handle<> coro) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  Entry entry;
  entry.type = IOUring::ACCEPT;
  entry.fd = fileDescriptor;
  entry.coro = coro;
  queue_.push_back(entry);
  AddEntries();
}

AcceptAwaiter IOUring::AcceptAsync(int fileDescriptor) {
  return AcceptAwaiter(*this, fileDescriptor);
}

WriteAwaiter IOUring::WriteAsync(int fileDescriptor, std::shared_ptr<std::string> data,
                                 size_t offset, size_t len) {
  return WriteAwaiter(*this, fileDescriptor, std::move(data), offset, len);
}

void IOUring::ProcessCalls() {
  io_uring_cqe *cqEntry;
  struct __kernel_timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
  
  int ret = io_uring_wait_cqe_timeout(&ring_, &cqEntry, &ts);
  
  if (ret == -ETIME || ret < 0 || !cqEntry) {
    return;
  }
  
  inProcess_--;
  SqeData *sqeData = (SqeData *)io_uring_cqe_get_data(cqEntry);
  
  if (!sqeData) {
    io_uring_cqe_seen(&ring_, cqEntry);
    return;
  }
  
  int fd = sqeData->fd;
  OpType opType = sqeData->opType;
  int result = cqEntry->res;
  std::coroutine_handle<> coroToResume = sqeData->coro;
  
  delete sqeData;
  io_uring_cqe_seen(&ring_, cqEntry);
  
  if (result < 0) {
    if (coroToResume && !coroToResume.done()) {
      if (opType == IOUring::ACCEPT) {
        auto promise = std::coroutine_handle<Promise>::from_address(coroToResume.address());
        promise.promise().acceptResult_ = result;
      } else if (opType == IOUring::READ) {
        auto promise = std::coroutine_handle<Promise>::from_address(coroToResume.address());
        promise.promise().readResult_ = 0;
      } else {
        auto promise = std::coroutine_handle<Promise>::from_address(coroToResume.address());
        promise.promise().writeResult_ = 0;
      }
      try {
        coroToResume.resume();
      } catch (...) {}
    }
    return;
  }
  
  if (opType == IOUring::ACCEPT) {
    auto promise = std::coroutine_handle<Promise>::from_address(coroToResume.address());
    promise.promise().acceptResult_ = result;
  } else if (opType == IOUring::READ) {
    auto promise = std::coroutine_handle<Promise>::from_address(coroToResume.address());
    promise.promise().readResult_ = result;
  } else {
    auto promise = std::coroutine_handle<Promise>::from_address(coroToResume.address());
    promise.promise().writeResult_ = static_cast<size_t>(result);
  }
  
  if (coroToResume && !coroToResume.done()) {
    try {
      coroToResume.resume();
    } catch (const std::exception &e) {
      std::cerr << "[ProcessCalls] Exception during resume: " << e.what() << std::endl;
    } catch (...) {
      std::cerr << "[ProcessCalls] Unknown exception during resume" << std::endl;
    }
  }
}

}
