#pragma once
#include <future>
namespace HTTP {
struct IAsio {
  virtual std::future<int> Read(int fileDescriptor,
                                std::array<char, 256> &buffer) = 0;
  virtual std::future<int> Write(int fileDescriptor, const std::string &data) = 0;
  virtual std::future<int> Accept(int fileDescriptor) = 0;

  virtual ~IAsio() = default;
};
using IAsioPtr = std::unique_ptr<IAsio>;
} // namespace HTTP
