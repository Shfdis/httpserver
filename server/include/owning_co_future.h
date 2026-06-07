#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace HTTP {
namespace detail {

template <typename T>
struct StackFutureControl {
  std::mutex mutex;
  std::atomic_bool ready{false};
  std::conditional_t<std::is_void_v<T>, bool, std::optional<T>> value;
  std::exception_ptr exception;
  using Callback = std::function<void()>;
  std::atomic<std::shared_ptr<Callback>> continuation{nullptr};
};

template <typename ControlBlock, typename Fn>
void RegisterContinuation(ControlBlock &control, Fn &&fn) {
  auto callback = std::make_shared<typename ControlBlock::Callback>(
      std::forward<Fn>(fn));

  if (control.ready.load(std::memory_order_acquire)) {
    (*callback)();
    return;
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
      (*callback)();
    }
  }
}

} // namespace detail

template <typename T>
class OwningCoFuture {
  using ControlBlock = detail::StackFutureControl<T>;

  ControlBlock control_;
  std::unique_lock<std::mutex> producerLock_{control_.mutex};

  template <typename Fn>
  void Complete(Fn &&store) {
    if (!producerLock_.owns_lock()) {
      throw std::runtime_error("Future already satisfied");
    }

    std::forward<Fn>(store)();
    control_.ready.store(true, std::memory_order_release);
    auto continuation =
        control_.continuation.exchange(nullptr, std::memory_order_acq_rel);
    producerLock_.unlock();

    if (continuation) {
      (*continuation)();
    }
  }

public:
  OwningCoFuture() = default;

  template <typename Starter,
            typename = std::enable_if_t<!std::is_same_v<
                std::decay_t<Starter>, OwningCoFuture>>>
  explicit OwningCoFuture(Starter &&starter) {
    std::forward<Starter>(starter)(*this);
  }

  OwningCoFuture(const OwningCoFuture &) = delete;
  OwningCoFuture &operator=(const OwningCoFuture &) = delete;
  OwningCoFuture(OwningCoFuture &&) = delete;
  OwningCoFuture &operator=(OwningCoFuture &&) = delete;

  bool valid() const noexcept { return true; }

  bool isReady() const noexcept {
    return control_.ready.load(std::memory_order_acquire);
  }

  T get() {
    if (!control_.ready.load(std::memory_order_acquire)) {
      control_.mutex.lock();
      control_.mutex.unlock();
    }

    if (control_.exception) {
      std::rethrow_exception(control_.exception);
    }
    if constexpr (!std::is_void_v<T>) {
      if (!control_.value) {
        throw std::runtime_error("Future has no value");
      }
      return *control_.value;
    }
  }

  T Get() { return get(); }

  template <typename U = T>
  std::enable_if_t<!std::is_void_v<U>, void> Set(U value) {
    Complete([this, value = std::move(value)]() mutable {
      control_.value = std::move(value);
    });
  }

  template <typename U = T>
  std::enable_if_t<!std::is_void_v<U>, void> set(U value) {
    Set(std::move(value));
  }

  template <typename U = T>
  std::enable_if_t<std::is_void_v<U>, void> Set() {
    Complete([this] { control_.value = true; });
  }

  template <typename U = T>
  std::enable_if_t<std::is_void_v<U>, void> set() {
    Set();
  }

  void SetException(std::exception_ptr exception) {
    Complete([this, exception = std::move(exception)]() mutable {
      control_.exception = std::move(exception);
    });
  }

  void setException(std::exception_ptr exception) {
    SetException(std::move(exception));
  }

  bool await_ready() const noexcept { return isReady(); }

  bool await_suspend(std::coroutine_handle<> awaiting) {
    auto callback = std::make_shared<typename ControlBlock::Callback>(
        [awaiting]() mutable {
          if (awaiting && !awaiting.done()) {
            awaiting.resume();
          }
        });

    if (control_.ready.load(std::memory_order_acquire)) {
      return false;
    }

    std::shared_ptr<typename ControlBlock::Callback> expected = nullptr;
    if (!control_.continuation.compare_exchange_strong(
            expected, callback, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      throw std::runtime_error("Tried to set the subscriber second time");
    }

    if (control_.ready.load(std::memory_order_acquire)) {
      expected = callback;
      if (control_.continuation.compare_exchange_strong(
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
          detail::RegisterContinuation(control_, std::move(run));
        });
  }

  template <typename T1, typename U = T,
            typename = std::enable_if_t<std::is_void_v<U>>, typename = void>
  OwningCoFuture<T1> Then(std::function<T1()> &&then) {
    return OwningCoFuture<T1>(
        [this, thenFn = std::move(then)](OwningCoFuture<T1> &next) mutable {
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
          detail::RegisterContinuation(control_, std::move(run));
        });
  }
};

} // namespace HTTP
