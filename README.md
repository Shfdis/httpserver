# HTTP Server

A lightweight HTTP/1.1 server written in C++20 using Linux io_uring for async I/O.

## Features

- io_uring based async I/O
- Trie-based URL routing
- Support for GET, POST, PUT, PATCH, DELETE methods
- Query parameter and header parsing

## Requirements

- Linux kernel 5.6+ (for io_uring)
- liburing
- CMake 3.12+
- C++20 compiler

## Building

```bash
cd example
cmake .
make
```

## Usage

```cpp
#include "http_server.h"

int main() {
  HTTP::ServerBuilder builder;
  builder.SetPort(8080);
  builder.SetThreads(4);
  
  builder.AddRequest(HTTP::GET, "/hello", [](const HTTP::RequestData& req) {
    HTTP::ResponseData res;
    res.status = 200;
    res.body = "Hello, World!";
    return res;
  });
  
  auto server = builder.Build();
  server.Start();
  
  // Keep running
  std::promise<void>().get_future().wait();
}
```
## Benchmarks

The `benchmarks/` directory contains comparison tests against a Rust Tokio server.

### Specs

- **Policy**: one `io_uring` per worker thread
- **OS**: Arch Linux, Linux 6.18.3-arch1-1
- **CPU**: Intel(R) Core(TM) Ultra 7 155H (22 CPUs)
- **RAM**: 30 GiB
- **Compiler**: GCC 15.2.1
- **liburing**: 2.13

### Results (C++ coroutine server vs Rust Tokio)

Run:

```bash
cd example
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=OFF
cmake --build build -j

cd ../benchmarks
DURATION=30s THREADS=4 CONNECTIONS=100 ./benchmark.sh
```

Output (2026-04-02 16:42:45 MSK, non-ASAN Release build):

- **GET /echo**
  - **C++**: 427149 req/s
  - **Tokio**: 500231 req/s
- **POST /echo**
  - **C++**: 387572 req/s
  - **Tokio**: 452589 req/s
