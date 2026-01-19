#include "read_iterator.h"
#include "http_error.h"
#include "io_uring.h"
#include "request_data.h"
#include <chrono>
#include <functional>
#include <string_view>
namespace HTTP {
ReadIterator::ReadIterator(IOUring &ring, int fileDescriptor) : ring_(ring) {
  auto result = ring_.Read(fileDescriptor, buffer_);
  length_ = result.get();
  position_ = 0;
  fileDescriptor_ = fileDescriptor;
  eof_ = (length_ == 0);
}
char ReadIterator::operator*() {
  if (eof_)
    return '\0';
  if (position_ >= length_) {
    auto result = ring_.Read(fileDescriptor_, buffer_);
    length_ = result.get();
    position_ = 0;
    if (length_ == 0) {
      eof_ = true;
      return '\0';
    }
  }
  return buffer_[position_];
}
ReadIterator &ReadIterator::operator++() {
  position_++;
  return *this;
}
void ReadIterator::Current(std::function<void(char)> callback) {
  if (eof_) {
    callback('\0');
    return;
  }
  if (position_ >= length_) {
    auto result = ring_.Read(fileDescriptor_, buffer_);
    length_ = result.get();
    position_ = 0;
    if (length_ == 0) {
      eof_ = true;
      callback('\0');
    }
  }
  callback(buffer_[position_]);
}
void ReadIterator::operator++(int) { position_++; }
Method ReadIterator::ParseMethod() {
  std::string methodString;
  int count{0};
  std::function<void(char)> processOne = [&](char currentChar) {
    if (count == 5 || !*this || currentChar == ' ') {
      return;
    }
    methodString += currentChar;
    count++;
    ++*this;
    Current(processOne);
  };
  Current(processOne);
  if (methodString == "PUT") {
    return PUT;
  }
  if (methodString == "POST") {
    return POST;
  }
  if (methodString == "DELETE") {
    return DELETE;
  }
  if (methodString == "PATCH") {
    return PATCH;
  }
  if (methodString == "GET") {
    return GET;
  }
  throw HTTPError(400, "Invalid request");
}
void ReadIterator::ParseVariables(RequestData &request) {
  Current([&](char currentChar) {
    if (currentChar != '?' && currentChar != ' ') {
      throw HTTPError(400, "Invalid request");
    }
  });

  enum { Name, Value } current = Name;
  std::string name;
  std::string *value;
  if (**this != '?') { // will call std::terminate on read overflow
    return;
  }
  ++*this;
  std::function<void(char)> processOne = [&](char currentChar) {
    if (!*this || currentChar == ' ')
      return;
    if (current == Name) {
      if (currentChar == '=') {
        if (name == "") {
          throw HTTPError(400, "Empty parameter name");
        }
        current = Value;
        request.params[name] = "";
        value = &request.params[name];
      } else {
        name.push_back(currentChar);
      }
    } else {
      if (currentChar == '&') {
        name = "";
        value = nullptr;
        current = Name;
      } else {
        value->push_back(currentChar);
      }
    }
    ++*this;
    Current(processOne);
  };
  Current(processOne);
  if (!*this) {
    throw HTTPError(400, "Empty parameter name");
  }
}
void ReadIterator::ParseHeaders(RequestData &request) {
  enum { Name, Value } current = Name;
  std::string name;
  std::string *value;
  char last = 'a';
  while (*this) {
    if (**this == '\r') {
      ++*this;
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
        request.headers[name] = "";
        value = &request.headers[name];
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
    ++*this;
  }
  if (!*this) {
    throw HTTPError(400, "Invalid message");
  }
}
void ReadIterator::ParseBody(RequestData &request) {
  auto it = request.headers.find("Content-Length");
  if (it != request.headers.end()) {
    try {
      size_t length = std::stoul(it->second);
      for (size_t i = 0; i < length; ++i) {
        if (!*this)
          break;
        request.body.push_back(**this);
        ++*this;
      }
    } catch (...) {
    }
    return;
  }

  auto it2 = request.headers.find("Transfer-Encoding");
  if (it2 != request.headers.end() && it2->second == "chunked") {
    // Chunked encoding not implemented in this simple server
    return;
  }

  // If no Content-Length and not chunked, assume no body for GET/DELETE/etc.
  if (request.method == GET || request.method == DELETE) {
    return;
  }

  while (*this) {
    request.body.push_back(**this);
    ++*this;
  }
}
} // namespace HTTP
