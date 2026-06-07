#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
namespace HTTP {
enum Method { GET, PUT, POST, PATCH, DELETE, HEAD, OPTIONS, UNKNOWN };

constexpr size_t kRoutableMethodCount = 7;

struct RequestData {
  std::unordered_map<std::string, std::string> headers;
  std::unordered_map<std::string, std::string> params;
  std::unordered_map<std::string, std::string> trailers;
  std::vector<std::pair<std::string, std::string>> rawHeaders;
  std::vector<std::string> urlVariables;
  Method method{UNKNOWN};
  std::string methodToken;
  std::string target;
  std::string path;
  std::string query;
  std::string version;
  std::string body;

  std::optional<std::string> Header(std::string_view name) const;
};
struct ResponseData {
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  unsigned short status{200};
};
} // namespace HTTP
