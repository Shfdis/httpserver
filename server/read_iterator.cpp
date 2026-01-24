#include "read_iterator.h"
#include "http_error.h"
#include <algorithm>
#include <string>
namespace HTTP {
ReadIterator::ReadIterator(IOUring &ring, int fd_) : ring_(ring), fd_(fd_), length_(0), position_(0) {
}

Coroutine ReadIterator::Ensure() {
  if (position_ >= length_) {
    length_ = co_await ring_.ReadAsync(fd_, buffer_);
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

Coroutine ReadIterator::operator++() {
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

Coroutine ReadIterator::ParseMethod(RequestData &data) {
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

Coroutine ReadIterator::ParseVariables(RequestData &data) {
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

Coroutine ReadIterator::ParseHeaders(RequestData &data) {
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

Coroutine ReadIterator::ParseBody(RequestData &data) {
  auto it = data.headers.find("Content-Length");
  if (it != data.headers.end()) {
    try {
      size_t length = std::stoul(it->second);
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
    } catch (...) {
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
