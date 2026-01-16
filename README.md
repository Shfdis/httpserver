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

The `benchmarks/` directory contains comparison tests against a Rust Tokio server. Benchmark scripts were written with assistance from Cursor.
