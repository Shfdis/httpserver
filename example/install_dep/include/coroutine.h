#pragma once
#include <coroutine>
#include <exception>
namespace HTTP {
struct Promise;
struct Coroutine;

struct Promise {
  std::coroutine_handle<> continuation_;
  std::exception_ptr exception_;
  int acceptResult_{-1};
  size_t readResult_{0};
  size_t writeResult_{0};
  static thread_local std::coroutine_handle<Promise> *currentCoro_;

  Coroutine get_return_object();
  std::suspend_always initial_suspend() noexcept { return {}; }
  struct FinalAwaiter {
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) const noexcept {
      if (h.promise().continuation_) {
        return h.promise().continuation_;
      }
      return std::noop_coroutine();
    }
    void await_resume() const noexcept {}
  };
  FinalAwaiter final_suspend() noexcept { return {}; }

  void return_void() noexcept {}
  void unhandled_exception() {
    exception_ = std::current_exception();
  }
};

struct Coroutine : std::coroutine_handle<Promise> {
  using promise_type = Promise;

  Coroutine() noexcept = default;

  Coroutine(std::coroutine_handle<Promise> h) noexcept 
    : std::coroutine_handle<Promise>(h) {}

  ~Coroutine() {
    if (*this && done()) {
      destroy();
    }
  }

  Coroutine(Coroutine&& other) noexcept 
    : std::coroutine_handle<Promise>(other) {
    static_cast<std::coroutine_handle<Promise>&>(other) = nullptr;
  }

  Coroutine& operator=(Coroutine&& other) noexcept {
    if (this != &other) {
      if (*this && done()) {
        destroy();
      }
      static_cast<std::coroutine_handle<Promise>&>(*this) = other;
      static_cast<std::coroutine_handle<Promise>&>(other) = nullptr;
    }
    return *this;
  }

  Coroutine(const Coroutine&) = delete;
  Coroutine& operator=(const Coroutine&) = delete;
  
  struct CoroutineAwaiter {
    std::coroutine_handle<Promise> coro_;

    bool await_ready() const noexcept {
      return !coro_ || coro_.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coro) noexcept {
      if (coro_ && !coro_.done()) {
        coro_.promise().continuation_ = awaiting_coro;
        return coro_;
      } else {
        return std::noop_coroutine();
      }
    }

    void await_resume() {
      if (coro_ && coro_.promise().exception_) {
        std::rethrow_exception(coro_.promise().exception_);
      }
    }
  };

  CoroutineAwaiter operator co_await() const noexcept {
    return CoroutineAwaiter{*this};
  }
};

inline Coroutine Promise::get_return_object() {
  return {Coroutine::from_promise(*this)};
}
}
