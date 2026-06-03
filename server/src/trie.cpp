#include "trie.h"
#include "http_error.h"
#include "request_data.h"
namespace HTTP {

size_t Trie::StringHash::operator()(std::string_view value) const noexcept {
  return std::hash<std::string_view>{}(value);
}

bool Trie::StringEqual::operator()(std::string_view lhs,
                                   std::string_view rhs) const noexcept {
  return lhs == rhs;
}

Trie::Node &Trie::Node::Move(std::string_view segment) {
  if (segment == "*") {
    if (!wildcard) {
      wildcard = std::make_unique<Node>();
    }
    return *wildcard;
  }

  auto child = children.find(segment);
  if (child == children.end()) {
    auto [inserted, _] =
        children.emplace(std::string{segment}, std::make_unique<Node>());
    child = inserted;
  }
  return *child->second;
}

void Trie::AddRequest(Method method, RespondType respond,
                      std::string_view path) {
  Node *current = root_.get();
  size_t position = path.starts_with('/') ? 1 : 0;
  while (position < path.size()) {
    const size_t next = path.find('/', position);
    const auto segment =
        next == std::string_view::npos
            ? path.substr(position)
            : path.substr(position, next - position);
    current = &current->Move(segment);
    if (next == std::string_view::npos) {
      break;
    }
    position = next + 1;
  }
  current->handlers[method] = respond;
}

RespondType Trie::Match(Method method, std::string_view path,
                        std::vector<std::string> &urlVariables) const {
  const Node *current = root_.get();
  urlVariables.clear();

  size_t position = path.starts_with('/') ? 1 : 0;
  while (position < path.size()) {
    const size_t next = path.find('/', position);
    const auto segment =
        next == std::string_view::npos
            ? path.substr(position)
            : path.substr(position, next - position);

    const auto child = current->children.find(segment);
    if (child != current->children.end()) {
      current = child->second.get();
    } else if (current->wildcard) {
      urlVariables.emplace_back(segment);
      current = current->wildcard.get();
    } else {
      throw HTTPError(404, "Not found");
    }

    if (next == std::string_view::npos) {
      break;
    }
    position = next + 1;
  }

  if (!current->handlers[method]) {
    throw HTTPError(404, "Not found");
  }
  return *current->handlers[method];
}

Trie::Trie(Trie &&rhs) { root_ = std::move(rhs.root_); }
Trie &Trie::operator=(Trie &&rhs) {
  root_ = std::move(rhs.root_);
  return *this;
}
} // namespace HTTP
