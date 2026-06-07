#pragma once
#include <exception>
#include <string>
namespace HTTP {
class HTTPError : public std::exception {
public:
  std::string message;
  const int status;
  HTTPError(int status, std::string message);
};
} // namespace HTTP
