#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <cassert>
#include <utility>

namespace HTTP {

template <typename T>
class CoPromise;

template <typename T>
class CoFuture {
  template <typename>
  friend class CoFuture;
  template <typename>
  friend class CoPromise;

  struct ControlBlock {
    std::mutex mutex;
    std::atomic_bool ready{false};
    std::atomic_bool futureRetrieved{false};
    std::atomic_bool continuationStarted{false};
    std::optional<T> value;
    std::exception_ptr exception;
    using Callback = std::function<void()>;
    std::atomic<std::shared_ptr<Callback>> continuation{nullptr};
  };

  std::shared_ptr<ControlBlock> control_;

  explicit CoFuture(std::shared_ptr<ControlBlock> control) : control_(std::move(control)) {}

public:
  CoFuture() = default;

  bool valid() const noexcept { return static_cast<bool>(control_); }

  bool isReady() const noexcept {
    return control_ && control_->ready.load(std::memory_order_acquire);
  }

  T get() {
    if (!control_) {
      throw std::logic_error("Invalid future");
    }

    if (!control_->ready.load(std::memory_order_acquire)) {
      control_->mutex.lock();
      control_->mutex.unlock();
    }

    if (control_->exception) {
      std::rethrow_exception(control_->exception);
    }
    if (!control_->value) {
      throw std::runtime_error("Future has no value");
    }
    return *control_->value;
  }

  T Get() { return get(); }

  template <typename T1> // Takes T and returns T1
  CoFuture<T1> Then(std::function<T1(T)> &&then) {
    if (!control_) {
      throw std::logic_error("Invalid future");
    }
    auto nextPromise = std::make_shared<CoPromise<T1>>(); 
    auto nextFuture = nextPromise->GetFuture();
    auto current = control_;
    auto run = [current, nextPromise, thenFn = std::move(then)]() mutable {
      assert(current->value);
      try {
        nextPromise->Set(thenFn(*current->value)); 
      }
      catch (...) {
        nextPromise->SetException(std::current_exception());
      }
    };
    auto callback = std::make_shared<typename ControlBlock::Callback>(std::move(run));
    if (control_->ready.load(std::memory_order_acquire)) {
      control_->continuationStarted.store(true, std::memory_order_release);
      (*callback)();
      return nextFuture;
    }

    std::shared_ptr<typename ControlBlock::Callback> expected = nullptr;
    if (!control_->continuation.compare_exchange_strong(expected, callback,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
      throw std::runtime_error("Tried to set the subscriber second time");
    }

    if (control_->ready.load(std::memory_order_acquire)) {
      expected = callback;
      if (control_->continuation.compare_exchange_strong(expected, nullptr,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
      control_->continuationStarted.store(true, std::memory_order_release);
        (*callback)();
      }
    } 
    return nextFuture;
  }
};
template <typename T>
class CoPromise {
  using ControlBlock = typename CoFuture<T>::ControlBlock;

  std::shared_ptr<ControlBlock> control_;
  std::unique_lock<std::mutex> producerLock_;

  template <typename>
  friend class CoFuture;

public:
  CoPromise() : control_(std::make_shared<ControlBlock>()), producerLock_(control_->mutex) {
    control_->continuationStarted.store(false, std::memory_order_release);
  }

  CoPromise(const CoPromise &) = delete;
  CoPromise &operator=(const CoPromise &) = delete;
  CoPromise(CoPromise &&) noexcept = default;
  CoPromise &operator=(CoPromise &&) noexcept = default;

  CoFuture<T> GetFuture() {
    if (!control_) {
      throw std::logic_error("Invalid promise");
    }
    bool expected = false;
    if (!control_->futureRetrieved.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_acquire)) {
      throw std::runtime_error("Future already retrieved");
    }
    return CoFuture<T>(control_);
  }

  CoFuture<T> getFuture() { return GetFuture(); }

  void Set(T value) {
    if (!producerLock_.owns_lock()) {
      throw std::runtime_error("Promise already satisfied");
    }

    control_->value = std::move(value);
    control_->ready.store(true, std::memory_order_release);
    auto continuation = control_->continuation.exchange(nullptr, std::memory_order_acq_rel);
    producerLock_.unlock();

    if (continuation) {
      (*continuation)();
    }
  }

  void set(T value) { Set(std::move(value)); }

  void SetException(std::exception_ptr exception) {
    if (!producerLock_.owns_lock()) {
      throw std::runtime_error("Promise already satisfied");
    }

    control_->exception = std::move(exception);
    control_->ready.store(true, std::memory_order_release);
    auto continuation = control_->continuation.exchange(nullptr, std::memory_order_acq_rel);
    producerLock_.unlock();

    if (continuation) {
      (*continuation)();
    }
  }

  void setException(std::exception_ptr exception) { SetException(std::move(exception)); }
};

} // namespace HTTP
