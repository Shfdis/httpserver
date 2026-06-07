#include "non_owning_co_future.h"
#include "owning_co_future.h"
#include <coroutine>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

HTTP::NonOwningCoFuture<int>
AwaitOwning(std::function<HTTP::OwningCoFuture<int>()> &makeFuture) {
  int value = co_await makeFuture();
  co_return value + 1;
}

HTTP::NonOwningCoFuture<void>
AwaitOwningVoid(std::function<HTTP::OwningCoFuture<void>()> &makeFuture) {
  co_await makeFuture();
  co_return;
}

HTTP::NonOwningCoFuture<int> ReturnValue() { co_return 7; }

HTTP::NonOwningCoFuture<int> ThrowValue() {
  throw std::runtime_error("boom");
  co_return 0;
}

HTTP::NonOwningCoFuture<int> AwaitOwningReference(HTTP::OwningCoFuture<int> &future) {
  int value = co_await future;
  co_return value + 2;
}

HTTP::NonOwningCoFuture<void>
AwaitOwningVoidReference(HTTP::OwningCoFuture<void> &future) {
  co_await future;
  co_return;
}
} // namespace

int main() {
  {
    std::function<void(int)> complete;
    std::function<HTTP::OwningCoFuture<int>()> makeFuture = [&] {
      return HTTP::OwningCoFuture<int>(
          [&](HTTP::OwningCoFuture<int> &future) {
            complete = [&future](int value) { future.Set(value); };
          });
    };

    auto task = AwaitOwning(makeFuture);
    Check(!task.isReady(), "owning int await suspends");
    complete(41);
    Check(task.Get() == 42, "owning int await resumes with value");
  }

  {
    std::function<void()> complete;
    std::function<HTTP::OwningCoFuture<void>()> makeFuture = [&] {
      return HTTP::OwningCoFuture<void>(
          [&](HTTP::OwningCoFuture<void> &future) {
            complete = [&future] { future.Set(); };
          });
    };

    auto task = AwaitOwningVoid(makeFuture);
    Check(!task.isReady(), "owning void await suspends");
    complete();
    task.Get();
    Check(task.isReady(), "owning void await resumes");
  }

  {
    auto task = ReturnValue();
    Check(task.Get() == 7, "non-owning coroutine returns value");
  }

  {
    auto task = ThrowValue();
    bool threw = false;
    try {
      (void)task.Get();
    } catch (const std::runtime_error &) {
      threw = true;
    }
    Check(threw, "non-owning coroutine propagates exception");
  }

  {
    HTTP::OwningCoFuture<int> root;
    auto next = root.Then<int>(std::function<int(int)>(
        [](int value) { return value + 10; }));
    root.Set(5);
    Check(next.Get() == 15, "owning Then maps value");
  }

  {
    HTTP::OwningCoFuture<void> root;
    auto next = root.Then<int>(std::function<int()>([] { return 3; }));
    root.Set();
    Check(next.Get() == 3, "owning void Then maps value");
  }

  {
    std::function<void(int)> complete;
    std::function<HTTP::OwningCoFuture<int>()> makeFuture = [&] {
      return HTTP::OwningCoFuture<int>(
          [&](HTTP::OwningCoFuture<int> &future) {
            complete = [&future](int value) { future.Set(value); };
          });
    };

    auto task = AwaitOwning(makeFuture);
    std::noop_coroutine_handle noop = std::noop_coroutine();
    Check(task.await_suspend(noop), "non-owning future accepts subscriber");
    bool threw = false;
    try {
      (void)task.await_suspend(noop);
    } catch (const std::runtime_error &) {
      threw = true;
    }
    Check(threw, "non-owning future rejects second subscriber");
  }

  {
    HTTP::OwningCoFuture<int> future;
    std::noop_coroutine_handle noop = std::noop_coroutine();
    Check(future.await_suspend(noop), "owning future accepts subscriber");
    bool threw = false;
    try {
      (void)future.await_suspend(noop);
    } catch (const std::runtime_error &) {
      threw = true;
    }
    Check(threw, "owning future rejects second subscriber");
  }

  {
    HTTP::OwningCoFuture<int> future;
    std::thread producer([&future] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      future.Set(19);
    });
    Check(future.Get() == 19, "owning int Get blocks across threads");
    producer.join();
  }

  {
    HTTP::OwningCoFuture<void> future;
    std::thread producer([&future] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      future.Set();
    });
    future.Get();
    Check(future.isReady(), "owning void Get blocks across threads");
    producer.join();
  }

  {
    HTTP::OwningCoFuture<int> future;
    std::thread producer([&future] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      future.SetException(std::make_exception_ptr(std::runtime_error("thread")));
    });
    bool threw = false;
    try {
      (void)future.Get();
    } catch (const std::runtime_error &) {
      threw = true;
    }
    producer.join();
    Check(threw, "owning exception wakes Get across threads");
  }

  {
    HTTP::OwningCoFuture<int> root;
    auto task = AwaitOwningReference(root);
    std::thread producer([&root] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      root.Set(40);
    });
    Check(task.Get() == 42, "non-owning int Get blocks through awaited future");
    producer.join();
  }

  {
    HTTP::OwningCoFuture<void> root;
    auto task = AwaitOwningVoidReference(root);
    std::thread producer([&root] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      root.Set();
    });
    task.Get();
    producer.join();
    Check(task.isReady(), "non-owning void Get blocks through awaited future");
  }

  return 0;
}
