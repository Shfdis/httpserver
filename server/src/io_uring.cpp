#include "io_uring.h"
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <liburing/io_uring.h>
#include <stdexcept>
#include <linux/errno.h>

namespace HTTP {

IOUring::~IOUring() {
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
        std::move(entry.complete), std::move(entry.writeData), entry.writeOffset, entry.writeLen};
    
    if (entry.type == IOUring::READ) [[likely]] {
      io_uring_prep_read(sqEntry, entry.fd, entry.toRead.value(), 256, 0);
    } else if (entry.type == IOUring::ACCEPT) {
      io_uring_prep_accept(sqEntry, entry.fd, nullptr, nullptr, 0);
    } else [[likely]] {
      const char *ptr = sqeData->writeData.data() + sqeData->writeOffset;
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
    
    if (inProcess_ > 0) {
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

void IOUring::Write(int fileDescriptor, std::string_view data, size_t offset,
                    size_t len, std::function<void(int)> complete) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  Entry entry;
  entry.type = IOUring::WRITE;
  entry.fd = fileDescriptor;
  entry.writeData = data;
  entry.writeOffset = offset;
  entry.writeLen = len;
  entry.complete = std::move(complete);
  queue_.push_back(std::move(entry));
  AddEntries();
}

void IOUring::Read(int fileDescriptor, std::array<char, 256> &buffer,
                   std::function<void(int)> complete) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  Entry entry;
  entry.type = IOUring::READ;
  entry.fd = fileDescriptor;
  entry.toRead = buffer.begin();
  entry.complete = std::move(complete);
  queue_.push_back(entry);
}

CoFuture<int> IOUring::ReadAsync(int fileDescriptor, std::array<char, 256> &buffer) {
  auto promise = std::make_shared<CoPromise<int>>();
  auto future = promise->GetFuture();
  Read(fileDescriptor, buffer, [promise](int result) {
    promise->Set(result);
  });
  return future;
}

void IOUring::Accept(int fileDescriptor, std::function<void(int)> complete) {
  if (fileDescriptor < 0) {
    throw std::runtime_error("Invalid file descriptor");
  }
  Entry entry;
  entry.type = IOUring::ACCEPT;
  entry.fd = fileDescriptor;
  entry.complete = std::move(complete);
  queue_.push_back(entry);
  AddEntries();
}

CoFuture<int> IOUring::AcceptAsync(int fileDescriptor) {
  auto promise = std::make_shared<CoPromise<int>>();
  auto future = promise->GetFuture();
  Accept(fileDescriptor, [promise](int result) {
    promise->Set(result);
  });
  return future;
}

CoFuture<int> IOUring::WriteAsync(int fileDescriptor, std::string_view data,
                                     size_t offset, size_t len) {
  auto promise = std::make_shared<CoPromise<int>>();
  auto future = promise->GetFuture();
  Write(fileDescriptor, data, offset, len, [promise](int result) {
    promise->Set(result);
  });
  return future;
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
  
  int result = cqEntry->res;
  auto complete = std::move(sqeData->complete);
  
  delete sqeData;
  io_uring_cqe_seen(&ring_, cqEntry);
  
  if (complete) {
    complete(result);
  }
}

}
