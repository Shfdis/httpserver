#include "read_iterator.h"
#include <algorithm>
#include <cerrno>
#include <system_error>

namespace HTTP {

namespace {
constexpr int kMaxIoAttempts = 3;

bool IsRetryableIoError(int result) {
  return result == -EINTR || result == -EAGAIN || result == -EWOULDBLOCK;
}
} // namespace

ReadIterator::ReadIterator(IOUring &ring, int fd) : ring_(ring), fd_(fd) {}

CoFuture<void> ReadIterator::operator++() {
  if (position_ < length_) {
    ++position_;
  }

  int attempts = 0;
  while (position_ >= length_ && !eof_) {
    int result = co_await ring_.ReadAsync(fd_, buffer_);
    if (result > 0) {
      length_ = static_cast<size_t>(result);
      position_ = 0;
      co_return;
    }
    if (result == 0) {
      length_ = 0;
      position_ = 0;
      eof_ = true;
      co_return;
    }
    ++attempts;
    if (!IsRetryableIoError(result) || attempts >= kMaxIoAttempts) {
      throw std::system_error(-result, std::generic_category(), "read failed");
    }
  }
  co_return;
}

char ReadIterator::operator*() const {
  if (position_ >= length_) {
    return '\0';
  }
  return buffer_[position_];
}

ReadIterator::operator bool() const { return position_ < length_; }

bool ReadIterator::Eof() const { return eof_; }

size_t ReadIterator::Available() const {
  if (position_ >= length_) {
    return 0;
  }
  return length_ - position_;
}

const char *ReadIterator::CurrentPtr() const {
  if (position_ >= length_) {
    return nullptr;
  }
  return buffer_.data() + position_;
}

void ReadIterator::Advance(size_t n) {
  position_ = std::min(length_, position_ + n);
}

} // namespace HTTP
