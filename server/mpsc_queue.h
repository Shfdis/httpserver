#pragma once
#include <atomic>
#include <concepts>
#include <stdexcept>
namespace HTTP {
template <typename T>
  requires std::copy_constructible<T>
class MPSCQueue {
  struct Node {
    T value;
    Node *next;
    ~Node() { delete next; }
  };
  std::atomic<Node *> producers_ = nullptr;
  Node *consumer_ = nullptr;
  std::atomic_uint64_t size_ = 0;

public:
  ~MPSCQueue() {
    delete consumer_;
    delete producers_.load();
  }
  void Push(const T &val) {
    Node *newNode = new Node({val, nullptr});
    Node *currentProd;
    do {
      currentProd = producers_.load();
      newNode->next = currentProd;
    } while (!producers_.compare_exchange_weak(currentProd, newNode));
    size_++;
  }
  T Consume() {
    size_--;
    if (!consumer_) {
      Node *toReverse = producers_.exchange(nullptr);
      while (toReverse) {
        auto next = toReverse->next;
        toReverse->next = consumer_;
        consumer_ = toReverse;
        toReverse = next;
      }
    }
    if (!consumer_) {
      throw std::runtime_error("Consume called on empty queue");
    }
    T answer = consumer_->value;
    auto toDelete = consumer_;
    consumer_ = consumer_->next;
    toDelete->next = nullptr;
    delete toDelete;
    return answer;
  }
  bool Empty() const { return size_ == 0; }
};
}; // namespace HTTP
