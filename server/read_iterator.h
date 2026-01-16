#pragma once
#include "iasio.h"
#include "request_data.h"
#include <array>
#include <optional>
namespace HTTP {
class ReadIterator {
private:
  IAsio &ring_;
  std::array<char, 256> buffer_;
  size_t position_;
  size_t length_;
  int fileDescriptor_;

public:
  ReadIterator(IAsio &ring_, int fileDescriptor);
  ReadIterator &operator++();
  void operator++(int);
  void ParseVariables(RequestData &data);
  void ParseHeaders(RequestData &data);
  Method ParseMethod();
  void ParseBody(RequestData &data);
  std::optional<char> operator*();
  using value_type = std::optional<char>;
  using difference_type = std::ptrdiff_t;
};
} // namespace HTTP
