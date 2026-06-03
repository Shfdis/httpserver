#include "read_iterator.h"
#include "http_error.h"
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
namespace HTTP {

namespace {
constexpr int kMaxIoAttempts = 3;

bool IsRetryableIoError(int result) {
  return result == -EINTR || result == -EAGAIN || result == -EWOULDBLOCK;
}
} // namespace

ReadIterator::ReadIterator(IOUring &ring, int fd_) : ring_(ring), fd_(fd_), length_(0), position_(0) {
}

CoFuture<void> ReadIterator::Ensure() {
  if (position_ >= length_) {
    for (int attempt = 1; attempt <= kMaxIoAttempts; ++attempt) {
      int result = co_await ring_.ReadAsync(fd_, buffer_);
      if (result > 0) {
        length_ = static_cast<size_t>(result);
        position_ = 0;
        co_return;
      }
      if (result == 0) {
        length_ = 0;
        position_ = 0;
        co_return;
      }
      if (!IsRetryableIoError(result) || attempt == kMaxIoAttempts) {
        throw std::system_error(-result, std::generic_category(), "read failed");
      }
    }
    position_ = 0;
  }
  co_return;
}

size_t ReadIterator::Available() const {
  if (position_ >= length_) return 0;
  return length_ - position_;
}

const char *ReadIterator::CurrentPtr() const {
  if (position_ >= length_) return nullptr;
  return buffer_.data() + position_;
}

void ReadIterator::Advance(size_t n) {
  position_ += n;
}

CoFuture<void> ReadIterator::operator++() {
  ++position_;
  co_return;
}

ReadIterator::operator bool() {
  return position_ < length_ && **this != '\0';
}

char ReadIterator::operator*() {
  if (position_ >= length_) {
    return '\0';
  }
  return buffer_.at(position_);
}

CoFuture<void> ReadIterator::ParseMethod(RequestData &data) {
  co_await Ensure();
  if (length_ == 0) {
    throw HTTPError(400, "Invalid request");
  }
  while (true) {
    co_await Ensure();
    if (!*this) {
      throw HTTPError(400, "Invalid request");
    }
    if (**this != '\r' && **this != '\n') {
      break;
    }
    co_await ++*this;
  }
  std::string methodString;
  int count{0};
  while (count < 5) {
    co_await Ensure();
    if (!*this) {
      throw HTTPError(400, "Invalid request");
    }
    if (**this == ' ') {
      break;
    }
    methodString += **this;
    count++;
    co_await ++*this;
  }
  if (methodString == "PUT") {
    data.method = PUT;
    co_return;
  }
  if (methodString == "POST") {
    data.method = POST;
    co_return;
  }
  if (methodString == "DELETE") {
    data.method = DELETE;
    co_return;
  }
  if (methodString == "PATCH") {
    data.method = PATCH;
    co_return;
  }
  if (methodString == "GET") {
    data.method = GET;
    co_return;
  }
  throw HTTPError(400, "Invalid request");
}

CoFuture<void> ReadIterator::ParseVariables(RequestData &data) {
  co_await Ensure();
  if (**this != '?' && **this != ' ') {
    throw HTTPError(400, "Invalid request");
  }
  enum { Name, Value } current = Name;
  std::string name;
  std::string *value;
  if (**this != '?') {
    co_return;
  }
  co_await ++*this;
  while (true) {
    co_await Ensure();
    if (!*this) {
      throw HTTPError(400, "Empty parameter name");
    }
    if (**this == ' ') {
      break;
    }
    if (current == Name) {
      if (**this == '=') {
        if (name == "") {
          throw HTTPError(400, "Empty parameter name");
        }
        current = Value;
        data.params[name] = "";
        value = &data.params[name];
      } else {
        name.push_back(**this);
      }
    } else {
      if (**this == '&') {
        name = "";
        value = nullptr;
        current = Name;
      } else {
        value->push_back(**this);
      }
    }
    co_await ++*this;
  }
  co_return;
}

CoFuture<void> ReadIterator::ParseHeaders(RequestData &data) {
  enum { Name, Value } current = Name;
  std::string name;
  std::string *value;
  char last = 'a';
  while (true) {
    co_await Ensure();
    if (!*this) {
      throw HTTPError(400, "Invalid message");
    }
    if (**this == '\r') {
      co_await ++*this;
      continue;
    }
    if (last == **this && last == '\n') {
      break;
    }
    if (current == Name) {
      if (**this == ':') {
        if (name == "") {
          throw HTTPError(400, "Empty header name");
        }
        current = Value;
        data.headers[name] = "";
        value = &data.headers[name];
      } else {
        name.push_back(**this);
      }
    } else {
      if (**this == '\n') {
        name = "";
        value = nullptr;
        current = Name;
      } else {
        value->push_back(**this);
      }
    }
    last = **this;
    co_await ++*this;
  }
  co_return;
}

CoFuture<void> ReadIterator::ParseBody(RequestData &data) {
  auto it = data.headers.find("Content-Length");
  if (it != data.headers.end()) {
    size_t length;
    try {
      length = std::stoul(it->second);
    } catch (const std::invalid_argument &) {
      co_return;
    } catch (const std::out_of_range &) {
      co_return;
    }
    data.body.clear();
    data.body.reserve(length);
    co_await Ensure();
    if (*this && (**this == '\n' || **this == '\r')) {
      co_await ++*this;
      co_await Ensure();
      if (*this && (**this == '\n' || **this == '\r')) {
        co_await ++*this;
      }
    }
    size_t remaining = length;
    while (remaining > 0) {
      co_await Ensure();
      if (!*this) break;
      size_t avail = Available();
      if (avail == 0) continue;
      size_t take = std::min(avail, remaining);
      data.body.append(CurrentPtr(), take);
      Advance(take);
      remaining -= take;
    }
    co_return;
  }

  auto it2 = data.headers.find("Transfer-Encoding");
  if (it2 != data.headers.end() && it2->second == "chunked") {
    co_return;
  }

  if (data.method == GET || data.method == DELETE) {
    co_return;
  }

  co_await Ensure();
  if (*this && (**this == '\n' || **this == '\r')) {
    co_await ++*this;
    co_await Ensure();
    if (*this && (**this == '\n' || **this == '\r')) {
      co_await ++*this;
    }
  }

  while (true) {
    co_await Ensure();
    if (!*this) break;
    data.body.push_back(**this);
    co_await ++*this;
  }
  co_return;
}
}
