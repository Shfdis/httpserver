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

struct QueryParams {
  using Storage = std::vector<std::pair<std::string, std::string>>;
  using iterator = Storage::iterator;
  using const_iterator = Storage::const_iterator;

  Storage values;

  void clear() { values.clear(); }
  void reserve(size_t count) { values.reserve(count); }
  iterator begin() { return values.begin(); }
  iterator end() { return values.end(); }
  const_iterator begin() const { return values.begin(); }
  const_iterator end() const { return values.end(); }

  iterator find(std::string_view name) {
    for (auto it = values.begin(); it != values.end(); ++it) {
      if (it->first == name) {
        return it;
      }
    }
    return values.end();
  }

  const_iterator find(std::string_view name) const {
    for (auto it = values.begin(); it != values.end(); ++it) {
      if (it->first == name) {
        return it;
      }
    }
    return values.end();
  }

  std::string &operator[](std::string_view name) {
    auto it = find(name);
    if (it != end()) {
      return it->second;
    }
    values.emplace_back(name, std::string{});
    return values.back().second;
  }

  void Set(std::string_view name, std::string_view value) {
    auto it = find(name);
    if (it != end()) {
      it->second.assign(value);
      return;
    }
    values.emplace_back(name, value);
  }
};

struct RequestData {
  QueryParams params;
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
  bool connectionClose{false};
  bool connectionKeepAlive{false};

  std::optional<std::string> Header(std::string_view name) const;
  void Clear() {
    params.clear();
    trailers.clear();
    rawHeaders.clear();
    urlVariables.clear();
    method = UNKNOWN;
    methodToken.clear();
    target.clear();
    path.clear();
    query.clear();
    version.clear();
    body.clear();
    connectionClose = false;
    connectionKeepAlive = false;
  }
};
struct ResponseData {
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  unsigned short status{200};
  void Clear() {
    headers.clear();
    body.clear();
    status = 200;
  }
};
} // namespace HTTP
