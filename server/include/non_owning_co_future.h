#pragma once

#include "owning_co_future.h"
#include <coroutine>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace HTTP {

template <typename T>
class NonOwningCoFuture;

template <typename T>
class NonOwningCoFutureCoroutinePromise {
  using ControlBlock = detail::StackFutureControl<T>;

  ControlBlock control_;
  std::unique_lock<std::mutex> producerLock_{control_.mutex};
  bool completed_{false};

  template <typename>
  friend class NonOwningCoFuture;

  template <typename Fn>
  void StoreCompletion(Fn &&store) {
    if (completed_) {
      throw std::runtime_error("Future already satisfied");
    }
    std::forward<Fn>(store)();
    completed_ = true;
  }

  void PublishCompletion() noexcept {
    if (!completed_) {
      StoreCompletion([this] {
        control_.exception =
            std::make_exception_ptr(std::runtime_error("Coroutine did not complete"));
      });
    }

    control_.ready.store(true, std::memory_order_release);
    auto continuation =
        control_.continuation.exchange(nullptr, std::memory_order_acq_rel);
    producerLock_.unlock();

    if (continuation) {
      (*continuation)();
    }
  }

public:
  NonOwningCoFuture<T> get_return_object() {
    return NonOwningCoFuture<T>(
        std::coroutine_handle<NonOwningCoFutureCoroutinePromise>::from_promise(
            *this));
  }

  std::suspend_never initial_suspend() noexcept { return {}; }

  struct FinalAwaiter {
    bool await_ready() noexcept { return false; }
    void await_resume() noexcept {}
    void await_suspend(
        std::coroutine_handle<NonOwningCoFutureCoroutinePromise> handle) noexcept {
      handle.promise().PublishCompletion();
    }
  };

  FinalAwaiter final_suspend() noexcept { return {}; }

  template <typename U>
  requires(!std::is_void_v<T>)
  void return_value(U &&value) {
    StoreCompletion([this, value = std::forward<U>(value)]() mutable {
      control_.value = std::move(value);
    });
  }

  void unhandled_exception() {
    StoreCompletion([this] { control_.exception = std::current_exception(); });
  }
};

template <>
class NonOwningCoFutureCoroutinePromise<void> {
  using ControlBlock = detail::StackFutureControl<void>;

  ControlBlock control_;
  std::unique_lock<std::mutex> producerLock_{control_.mutex};
  bool completed_{false};

  template <typename>
  friend class NonOwningCoFuture;

  template <typename Fn>
  void StoreCompletion(Fn &&store) {
    if (completed_) {
      throw std::runtime_error("Future already satisfied");
    }
    std::forward<Fn>(store)();
    completed_ = true;
  }

  void PublishCompletion() noexcept {
    if (!completed_) {
      StoreCompletion([this] {
        control_.exception =
            std::make_exception_ptr(std::runtime_error("Coroutine did not complete"));
      });
    }

    control_.ready.store(true, std::memory_order_release);
    auto continuation =
        control_.continuation.exchange(nullptr, std::memory_order_acq_rel);
    producerLock_.unlock();

    if (continuation) {
      (*continuation)();
    }
  }

public:
  NonOwningCoFuture<void> get_return_object();

  std::suspend_never initial_suspend() noexcept { return {}; }

  struct FinalAwaiter {
    bool await_ready() noexcept { return false; }
    void await_resume() noexcept {}
    void await_suspend(
        std::coroutine_handle<NonOwningCoFutureCoroutinePromise> handle) noexcept {
      handle.promise().PublishCompletion();
    }
  };

  FinalAwaiter final_suspend() noexcept { return {}; }

  void return_void() { StoreCompletion([this] { control_.value = true; }); }

  void unhandled_exception() {
    StoreCompletion([this] { control_.exception = std::current_exception(); });
  }
};

template <typename T>
class NonOwningCoFuture {
  using Promise = NonOwningCoFutureCoroutinePromise<T>;
  using Handle = std::coroutine_handle<Promise>;
  using ControlBlock = typename Promise::ControlBlock;

  Handle handle_{};

  explicit NonOwningCoFuture(Handle handle) : handle_(handle) {}

  template <typename>
  friend class NonOwningCoFutureCoroutinePromise;

  Promise *promise() const {
    if (!handle_) {
      throw std::logic_error("Invalid future");
    }
    return &handle_.promise();
  }

public:
  NonOwningCoFuture() = default;
  NonOwningCoFuture(const NonOwningCoFuture &) = delete;
  NonOwningCoFuture &operator=(const NonOwningCoFuture &) = delete;

  NonOwningCoFuture(NonOwningCoFuture &&rhs) noexcept
      : handle_(std::exchange(rhs.handle_, {})) {}

  NonOwningCoFuture &operator=(NonOwningCoFuture &&rhs) noexcept {
    if (this != &rhs) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(rhs.handle_, {});
    }
    return *this;
  }

  ~NonOwningCoFuture() {
    if (handle_) {
      handle_.destroy();
    }
  }

  bool valid() const noexcept { return static_cast<bool>(handle_); }

  bool isReady() const noexcept {
    if (!handle_) {
      return false;
    }
    return handle_.promise().control_.ready.load(std::memory_order_acquire);
  }

  T get() {
    Promise *p = promise();
    ControlBlock &control = p->control_;
    if (!control.ready.load(std::memory_order_acquire)) {
      control.mutex.lock();
      control.mutex.unlock();
    }

    if (control.exception) {
      std::rethrow_exception(control.exception);
    }
    if constexpr (!std::is_void_v<T>) {
      if (!control.value) {
        throw std::runtime_error("Future has no value");
      }
      return *control.value;
    }
  }

  T Get() { return get(); }

  bool await_ready() const noexcept { return isReady(); }

  bool await_suspend(std::coroutine_handle<> awaiting) {
    ControlBlock &control = promise()->control_;
    auto callback = std::make_shared<typename ControlBlock::Callback>(
        [awaiting]() mutable {
          if (awaiting && !awaiting.done()) {
            awaiting.resume();
          }
        });

    if (control.ready.load(std::memory_order_acquire)) {
      return false;
    }

    std::shared_ptr<typename ControlBlock::Callback> expected = nullptr;
    if (!control.continuation.compare_exchange_strong(
            expected, callback, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      throw std::runtime_error("Tried to set the subscriber second time");
    }

    if (control.ready.load(std::memory_order_acquire)) {
      expected = callback;
      if (control.continuation.compare_exchange_strong(
              expected, nullptr, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return false;
      }
    }
    return true;
  }

  T await_resume() { return get(); }

  template <typename T1, typename U = T,
            typename = std::enable_if_t<!std::is_void_v<U>>>
  OwningCoFuture<T1> Then(std::function<T1(U)> &&then) {
    return OwningCoFuture<T1>(
        [this, thenFn = std::move(then)](OwningCoFuture<T1> &next) mutable {
          ControlBlock &control = promise()->control_;
          auto run = [this, &next, thenFn = std::move(thenFn)]() mutable {
            try {
              if constexpr (std::is_void_v<T1>) {
                thenFn(get());
                next.Set();
              } else {
                next.Set(thenFn(get()));
              }
            } catch (...) {
              next.SetException(std::current_exception());
            }
          };
          detail::RegisterContinuation(control, std::move(run));
        });
  }

  template <typename T1, typename U = T,
            typename = std::enable_if_t<std::is_void_v<U>>, typename = void>
  OwningCoFuture<T1> Then(std::function<T1()> &&then) {
    return OwningCoFuture<T1>(
        [this, thenFn = std::move(then)](OwningCoFuture<T1> &next) mutable {
          ControlBlock &control = promise()->control_;
          auto run = [this, &next, thenFn = std::move(thenFn)]() mutable {
            try {
              get();
              if constexpr (std::is_void_v<T1>) {
                thenFn();
                next.Set();
              } else {
                next.Set(thenFn());
              }
            } catch (...) {
              next.SetException(std::current_exception());
            }
          };
          detail::RegisterContinuation(control, std::move(run));
        });
  }
};

inline NonOwningCoFuture<void>
NonOwningCoFutureCoroutinePromise<void>::get_return_object() {
  return NonOwningCoFuture<void>(
      std::coroutine_handle<NonOwningCoFutureCoroutinePromise>::from_promise(
          *this));
}

} // namespace HTTP

template <typename T, typename... Args>
struct std::coroutine_traits<HTTP::NonOwningCoFuture<T>, Args...> {
  using promise_type = HTTP::NonOwningCoFutureCoroutinePromise<T>;
};
