#pragma once
#include "request_data.h"
#include <array>
namespace HTTP {
class IOUring;
class ReadIterator {
private:
  IOUring &ring_;
  std::array<char, 256> buffer_;
  size_t position_;
  size_t length_;
  int fileDescriptor_;
  bool eof_ = false;

public:
  ReadIterator(IOUring &ring, int fileDescriptor);
  ReadIterator &operator++();
  void operator++(int);
  void ParseVariables(RequestData &data);
  void ParseHeaders(RequestData &data);
  Method ParseMethod();
  void ParseBody(RequestData &data);
  char operator*();
  explicit operator bool() const { return !eof_; }
  using value_type = char;
  using difference_type = std::ptrdiff_t;
};
} // namespace HTTP
