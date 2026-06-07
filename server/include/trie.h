#pragma once
#include "request_data.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace HTTP {
using RespondType = std::function<ResponseData(const RequestData &)>;
class Trie {
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view value) const noexcept;
  };
  struct StringEqual {
    using is_transparent = void;
    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
  };
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>, StringHash,
                       StringEqual>
        children;
    std::unique_ptr<Node> wildcard;
    std::optional<RespondType> handlers[kRoutableMethodCount];
    Node() = default;
    Node &Move(std::string_view segment);
  };
  std::unique_ptr<Node> root_ = std::make_unique<Node>();

public:
  struct RouteResult {
    bool pathFound{false};
    bool methodAllowed{false};
    bool automaticOptions{false};
    RespondType handler;
    std::string allow;
  };

  Trie() = default;
  Trie(Trie &&rhs);
  Trie &operator=(Trie &&rhs);
  void AddRequest(Method type, RespondType function, std::string_view path);
  std::string AllAllowedMethods() const;
  RouteResult Resolve(Method method, std::string_view path,
                      std::vector<std::string> &urlVariables) const;
  RespondType Match(Method method, std::string_view path,
                    std::vector<std::string> &urlVariables) const;
};
} // namespace HTTP
