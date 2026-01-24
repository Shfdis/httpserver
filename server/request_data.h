#pragma once
#include <string>
#include <unordered_map>
#include <vector>
namespace HTTP {
enum Method { GET, PUT, POST, PATCH, DELETE };
struct RequestData {
  std::unordered_map<std::string, std::string> headers;
  std::unordered_map<std::string, std::string> params;
  std::vector<std::string> urlVariables;
  Method method;
  std::string body;
};
struct ResponseData {
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  unsigned short status;
};
} // namespace HTTP
