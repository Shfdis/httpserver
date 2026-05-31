#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <cassert>
#include <thread>
#include <type_traits>
#include <utility>

namespace HTTP {

template <typename T>
class CoPromise;

template <typename T>
class CoFutureAwaiter;

template <typename T>
class CoFuture {
  template <typename>
  friend class CoFuture;
  template <typename>
  friend class CoPromise;
  template <typename>
  friend class CoFutureAwaiter;

  struct ControlBlock {
    std::mutex mutex;
    std::atomic_bool ready{false};
    std::atomic_bool futureRetrieved{false};
    std::conditional_t<std::is_void_v<T>, bool, std::optional<T>> value;
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
    if constexpr (!std::is_void_v<T>) {
      if (!control_->value) {
        throw std::runtime_error("Future has no value");
      }
      return *control_->value;
    }
  }

  T Get() { return get(); }

  template <typename T1, typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  CoFuture<T1> Then(std::function<T1(U)> &&then) {
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
  CoPromise() : control_(std::make_shared<ControlBlock>()), producerLock_(control_->mutex) {}

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

template <>
class CoPromise<void> {
  using ControlBlock = typename CoFuture<void>::ControlBlock;

  std::shared_ptr<ControlBlock> control_;
  std::unique_lock<std::mutex> producerLock_;

public:
  CoPromise() : control_(std::make_shared<ControlBlock>()), producerLock_(control_->mutex) {}

  CoPromise(const CoPromise &) = delete;
  CoPromise &operator=(const CoPromise &) = delete;
  CoPromise(CoPromise &&) noexcept = default;
  CoPromise &operator=(CoPromise &&) noexcept = default;

  CoFuture<void> GetFuture() {
    if (!control_) {
      throw std::logic_error("Invalid promise");
    }
    bool expected = false;
    if (!control_->futureRetrieved.compare_exchange_strong(expected, true,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_acquire)) {
      throw std::runtime_error("Future already retrieved");
    }
    return CoFuture<void>(control_);
  }

  CoFuture<void> getFuture() { return GetFuture(); }

  void Set() {
    if (!producerLock_.owns_lock()) {
      throw std::runtime_error("Promise already satisfied");
    }
    control_->ready.store(true, std::memory_order_release);
    auto continuation = control_->continuation.exchange(nullptr, std::memory_order_acq_rel);
    producerLock_.unlock();
    if (continuation) {
      (*continuation)();
    }
  }

  void set() { Set(); }

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

template <typename T>
class CoFutureAwaiter {
  using ControlBlock = typename CoFuture<T>::ControlBlock;

  std::shared_ptr<ControlBlock> control_;

public:
  explicit CoFutureAwaiter(const CoFuture<T> &future) : control_(future.control_) {}
  explicit CoFutureAwaiter(CoFuture<T> &&future) : control_(std::move(future.control_)) {}

  bool await_ready() const noexcept {
    return !control_ || control_->ready.load(std::memory_order_acquire);
  }

  bool await_suspend(std::coroutine_handle<> awaiting) {
    auto callback = std::make_shared<typename ControlBlock::Callback>(
        [awaiting]() mutable {
          if (awaiting && !awaiting.done()) {
            awaiting.resume();
          }
        });

    if (control_->ready.load(std::memory_order_acquire)) {
      return false;
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
        return false;
      }
    }
    return true;
  }

  T await_resume() {
    return CoFuture<T>(std::move(control_)).get();
  }
};

template <typename T>
CoFutureAwaiter<T> operator co_await(const CoFuture<T> &future) {
  return CoFutureAwaiter<T>(future);
}

template <typename T>
CoFutureAwaiter<T> operator co_await(CoFuture<T> &&future) {
  return CoFutureAwaiter<T>(std::move(future));
}

template <typename T>
class CoFutureCoroutinePromise {
  CoPromise<T> promise_;

public:
  CoFuture<T> get_return_object() {
    return promise_.GetFuture();
  }

  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  template <typename U>
  void return_value(U &&value) {
    promise_.Set(std::forward<U>(value));
  }

  void unhandled_exception() {
    promise_.SetException(std::current_exception());
  }
};

template <>
class CoFutureCoroutinePromise<void> {
  CoPromise<void> promise_;

public:
  CoFuture<void> get_return_object() {
    return promise_.GetFuture();
  }

  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  void return_void() {
    promise_.Set();
  }

  void unhandled_exception() {
    promise_.SetException(std::current_exception());
  }
};

template <typename F>
auto RunCoroInThread(F &&func) -> CoFuture<std::invoke_result_t<std::decay_t<F> &>> {
  using Func = std::decay_t<F>;
  using T = std::invoke_result_t<Func &>;

  auto promise = std::make_shared<CoPromise<T>>();
  auto future = promise->GetFuture();
  try {
    std::thread([promise, func = std::forward<F>(func)]() mutable {
      try {
        if constexpr (std::is_void_v<T>) {
          std::invoke(func);
          promise->Set();
        } else {
          promise->Set(std::invoke(func));
        }
      } catch (...) {
        promise->SetException(std::current_exception());
      }
    }).detach();
  } catch (...) {
    promise->SetException(std::current_exception());
  }
  return future;
}

}

template <typename T, typename... Args>
struct std::coroutine_traits<HTTP::CoFuture<T>, Args...> {
  using promise_type = HTTP::CoFutureCoroutinePromise<T>;
};
