#include "trie.h"
#include "http_error.h"
#include "request_data.h"
#include <array>
namespace HTTP {
namespace {
bool IsRoutable(Method method) {
  return method >= GET && method <= OPTIONS;
}

size_t MethodIndex(Method method) { return static_cast<size_t>(method); }

const char *MethodName(Method method) {
  switch (method) {
  case GET:
    return "GET";
  case PUT:
    return "PUT";
  case POST:
    return "POST";
  case PATCH:
    return "PATCH";
  case DELETE:
    return "DELETE";
  case HEAD:
    return "HEAD";
  case OPTIONS:
    return "OPTIONS";
  case UNKNOWN:
    return "";
  }
  return "";
}

void AppendAllow(std::string &allow, Method method) {
  if (!allow.empty()) {
    allow += ", ";
  }
  allow += MethodName(method);
}
} // namespace

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
  if (!IsRoutable(method)) {
    return;
  }
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
  current->handlers[MethodIndex(method)] = respond;
}

std::string Trie::AllAllowedMethods() const {
  std::array<bool, kRoutableMethodCount> methods{};
  auto visit = [&](const Node *node, const auto &self) -> void {
    for (size_t i = 0; i < kRoutableMethodCount; ++i) {
      if (node->handlers[i]) {
        methods[i] = true;
      }
    }
    for (const auto &[_, child] : node->children) {
      self(child.get(), self);
    }
    if (node->wildcard) {
      self(node->wildcard.get(), self);
    }
  };
  visit(root_.get(), visit);

  std::string allow;
  if (methods[MethodIndex(GET)]) {
    AppendAllow(allow, GET);
  }
  if (methods[MethodIndex(HEAD)] || methods[MethodIndex(GET)]) {
    AppendAllow(allow, HEAD);
  }
  AppendAllow(allow, OPTIONS);
  if (methods[MethodIndex(POST)]) {
    AppendAllow(allow, POST);
  }
  if (methods[MethodIndex(PUT)]) {
    AppendAllow(allow, PUT);
  }
  if (methods[MethodIndex(PATCH)]) {
    AppendAllow(allow, PATCH);
  }
  if (methods[MethodIndex(DELETE)]) {
    AppendAllow(allow, DELETE);
  }
  return allow;
}

Trie::RouteResult Trie::Resolve(
    Method method, std::string_view path,
    std::vector<std::string> &urlVariables) const {
  auto hasHandler = [](const Node &node, Method candidate) {
    return IsRoutable(candidate) &&
           node.handlers[MethodIndex(candidate)].has_value();
  };
  auto hasAnyHandler = [](const Node &node) {
    for (const auto &handler : node.handlers) {
      if (handler) {
        return true;
      }
    }
    return false;
  };
  auto buildAllow = [&](const Node &node) {
    std::string allow;
    if (hasHandler(node, GET)) {
      AppendAllow(allow, GET);
    }
    if (hasHandler(node, HEAD) || hasHandler(node, GET)) {
      AppendAllow(allow, HEAD);
    }
    AppendAllow(allow, OPTIONS);
    if (hasHandler(node, POST)) {
      AppendAllow(allow, POST);
    }
    if (hasHandler(node, PUT)) {
      AppendAllow(allow, PUT);
    }
    if (hasHandler(node, PATCH)) {
      AppendAllow(allow, PATCH);
    }
    if (hasHandler(node, DELETE)) {
      AppendAllow(allow, DELETE);
    }
    return allow;
  };

  RouteResult result;
  const Node *node = root_.get();
  urlVariables.clear();
  size_t position = path.starts_with('/') ? 1 : 0;
  while (position < path.size()) {
    const size_t next = path.find('/', position);
    const auto segment =
        next == std::string_view::npos
            ? path.substr(position)
            : path.substr(position, next - position);

    const auto child = node->children.find(segment);
    if (child != node->children.end()) {
      node = child->second.get();
    } else if (node->wildcard) {
      urlVariables.emplace_back(segment);
      node = node->wildcard.get();
    } else {
      node = nullptr;
      break;
    }

    if (next == std::string_view::npos) {
      break;
    }
    position = next + 1;
  }

  if (!node || !hasAnyHandler(*node)) {
    urlVariables.clear();
    return result;
  }

  result.pathFound = true;
  result.allow = buildAllow(*node);
  if (!IsRoutable(method)) {
    return result;
  }

  if (hasHandler(*node, method)) {
    result.methodAllowed = true;
    result.handler = *node->handlers[MethodIndex(method)];
    return result;
  }
  if (method == HEAD && hasHandler(*node, GET)) {
    result.methodAllowed = true;
    result.handler = *node->handlers[MethodIndex(GET)];
    return result;
  }
  if (method == OPTIONS) {
    result.methodAllowed = true;
    result.automaticOptions = true;
    return result;
  }
  return result;
}

RespondType Trie::Match(Method method, std::string_view path,
                        std::vector<std::string> &urlVariables) const {
  RouteResult result = Resolve(method, path, urlVariables);
  if (!result.pathFound || !result.methodAllowed || result.automaticOptions) {
    throw HTTPError(404, "Not found");
  }
  return result.handler;
}

Trie::Trie(Trie &&rhs) { root_ = std::move(rhs.root_); }
Trie &Trie::operator=(Trie &&rhs) {
  root_ = std::move(rhs.root_);
  return *this;
}
} // namespace HTTP
