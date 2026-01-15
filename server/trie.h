#pragma once
#include "request_data.h"
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
namespace HTTP {
using RespondType = std::function<ResponseData(const RequestData &)>;
class Trie {
  struct Node {
    std::unordered_map<char, std::unique_ptr<Node>> children;
    std::optional<RespondType> handlers[5];
    Node() = default;
    Node &Move(char c);
    const Node &Move(char c) const;
  };
  std::unique_ptr<Node> root_ = std::make_unique<Node>();

public:
  Trie() = default;
  Trie(Trie &&rhs);
  Trie &operator=(Trie &&rhs);
  const Node &GetRoot();
  void AddRequest(Method type, RespondType function, std::string_view path);
};
} // namespace HTTP
