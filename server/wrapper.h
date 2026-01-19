#pragma once
#include <utility>
template <typename T, typename V> auto operator+(T &&f, V &&g) {
  return [f = std::forward(f), g = std::forward(g)] {
    f();
    g();
  };
}
