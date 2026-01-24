#include "trie.h"
#include "http_error.h"
#include "request_data.h"
namespace HTTP {
Trie::Node &Trie::Node::Move(char c) {
  if (c == '*') {
    any = true;
    return *this;
  }
  if (!children.contains(c)) {
    children[c] = std::make_unique<Node>();
  }
  return *children[c];
}
const Trie::Node &Trie::Node::Move(char c) const {
  try {
    return *children.at(c);
  } catch (...) {
    throw HTTPError(404, "Not found");
  }
}
void Trie::AddRequest(Method method, RespondType respond,
                      std::string_view path) {
  Node *current = root_.get();
  for (auto c : path) {
    current = &current->Move(c);
  }
  current->handlers[method] = respond;
}
const Trie::Node &Trie::GetRoot() { return *root_; }
Trie::Trie(Trie &&rhs) { root_ = std::move(rhs.root_); }
Trie &Trie::operator=(Trie &&rhs) {
  root_ = std::move(rhs.root_);
  return *this;
}
} // namespace HTTP
