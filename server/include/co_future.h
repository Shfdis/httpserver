#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <cassert>
#include <thread>
#include <type_traits>
#include <utility>

namespace HTTP {

class IOUring;

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
  friend class IOUring;

  using StoredValue = std::conditional_t<std::is_void_v<T>, bool, T>;

  struct ControlBlock {
    std::atomic_bool ready{false};
    std::atomic_bool satisfied{false};
    std::atomic_bool futureRetrieved{false};
    std::atomic_uint waiters{0};
    std::conditional_t<std::is_void_v<T>, bool, std::optional<T>> value;
    std::exception_ptr exception;
    using Callback = std::function<void()>;
    std::atomic_uintptr_t continuation{0};
  };

  std::shared_ptr<ControlBlock> control_;

  explicit CoFuture(std::shared_ptr<ControlBlock> control) : control_(std::move(control)) {}

  static constexpr uintptr_t kCallbackTag = 1;

  static void RunContinuation(uintptr_t continuation) {
    if (continuation == 0) {
      return;
    }
    if ((continuation & kCallbackTag) != 0) {
      std::unique_ptr<typename ControlBlock::Callback> callback(
          reinterpret_cast<typename ControlBlock::Callback *>(
              continuation & ~kCallbackTag));
      (*callback)();
      return;
    }
    auto handle =
        std::coroutine_handle<>::from_address(reinterpret_cast<void *>(continuation));
    if (handle && !handle.done()) {
      handle.resume();
    }
  }

  static void PublishReady(const std::shared_ptr<ControlBlock> &control) {
    control->ready.store(true, std::memory_order_release);
    auto continuation = control->continuation.exchange(0, std::memory_order_acq_rel);
    if (control->waiters.load(std::memory_order_acquire) > 0) {
      control->ready.notify_all();
    }
    RunContinuation(continuation);
  }

  static void CompleteValue(const std::shared_ptr<ControlBlock> &control,
                            StoredValue value = StoredValue{}) {
    if (control->satisfied.exchange(true, std::memory_order_acq_rel)) {
      throw std::runtime_error("Promise already satisfied");
    }
    if constexpr (std::is_void_v<T>) {
      control->value = true;
    } else {
      control->value = std::move(value);
    }
    PublishReady(control);
  }

  static void CompleteException(const std::shared_ptr<ControlBlock> &control,
                                std::exception_ptr exception) {
    if (control->satisfied.exchange(true, std::memory_order_acq_rel)) {
      throw std::runtime_error("Promise already satisfied");
    }
    control->exception = std::move(exception);
    PublishReady(control);
  }

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

    while (!control_->ready.load(std::memory_order_acquire)) {
      control_->waiters.fetch_add(1, std::memory_order_acq_rel);
      if (!control_->ready.load(std::memory_order_acquire)) {
        control_->ready.wait(false, std::memory_order_acquire);
      }
      control_->waiters.fetch_sub(1, std::memory_order_acq_rel);
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
    auto callback =
        std::make_unique<typename ControlBlock::Callback>(std::move(run));
    if (control_->ready.load(std::memory_order_acquire)) {
      (*callback)();
      return nextFuture;
    }

    uintptr_t raw = reinterpret_cast<uintptr_t>(callback.get());
    assert((raw & kCallbackTag) == 0);
    raw |= kCallbackTag;
    uintptr_t expected = 0;
    if (!control_->continuation.compare_exchange_strong(expected, raw,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
      throw std::runtime_error("Tried to set the subscriber second time");
    }
    callback.release();

    if (control_->ready.load(std::memory_order_acquire)) {
      expected = raw;
      if (control_->continuation.compare_exchange_strong(expected, 0,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
        RunContinuation(raw);
      }
    } 
    return nextFuture;
  }
};

template <typename T>
class CoPromise {
  using ControlBlock = typename CoFuture<T>::ControlBlock;

  std::shared_ptr<ControlBlock> control_;

  template <typename>
  friend class CoFuture;

public:
  CoPromise() : control_(std::make_shared<ControlBlock>()) {}

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
    CoFuture<T>::CompleteValue(control_, std::move(value));
  }

  void set(T value) { Set(std::move(value)); }

  void SetException(std::exception_ptr exception) {
    CoFuture<T>::CompleteException(control_, std::move(exception));
  }

  void setException(std::exception_ptr exception) { SetException(std::move(exception)); }
};

template <>
class CoPromise<void> {
  using ControlBlock = typename CoFuture<void>::ControlBlock;

  std::shared_ptr<ControlBlock> control_;

public:
  CoPromise() : control_(std::make_shared<ControlBlock>()) {}

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
    CoFuture<void>::CompleteValue(control_, true);
  }

  void set() { Set(); }

  void SetException(std::exception_ptr exception) {
    CoFuture<void>::CompleteException(control_, std::move(exception));
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
    if (control_->ready.load(std::memory_order_acquire)) {
      return false;
    }

    uintptr_t raw = reinterpret_cast<uintptr_t>(awaiting.address());
    assert((raw & CoFuture<T>::kCallbackTag) == 0);
    uintptr_t expected = 0;
    if (!control_->continuation.compare_exchange_strong(expected, raw,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
      throw std::runtime_error("Tried to set the subscriber second time");
    }

    if (control_->ready.load(std::memory_order_acquire)) {
      expected = raw;
      if (control_->continuation.compare_exchange_strong(expected, 0,
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
