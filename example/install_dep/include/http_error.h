#pragma once
#include <exception>
#include <string_view>
namespace HTTP {
class HTTPError : public std::exception {
public:
  std::string_view message;
  const int status;
  HTTPError(int status, std::string_view message);
};
} // namespace HTTP
