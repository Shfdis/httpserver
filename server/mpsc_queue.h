#pragma once
#include <atomic>
#include <memory>
namespace HTTP {
template <typename T> class MPSCQueue {
  struct Node {
    T value;
    std::unique_ptr<Node> next;
  };
  std::atomic<std::unique_ptr<Node>> producers_;
  std::unique_ptr<Node> consumer_;

public:
  void Push(const T &val);
  T Consume();
};
}; // namespace HTTP
