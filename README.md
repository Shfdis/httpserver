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
  server.Start().Get();
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

Output (non-ASAN Release build):

#### GET /echo

| Server | Latency avg | Latency stdev | Latency max | +/- stdev | Requests/sec | Transfer/sec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| C++ coroutine server | 132.16us | 66.78us | 4.97ms | 80.82% | 439792.17 | 29.78MB |
| Rust Tokio server | 150.12us | 119.34us | 5.53ms | 94.01% | 506996.06 | 40.61MB |

#### POST /echo

| Server | Latency avg | Latency stdev | Latency max | +/- stdev | Requests/sec | Transfer/sec |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| C++ coroutine server | 149.98us | 77.14us | 5.27ms | 81.01% | 402624.34 | 29.57MB |
| Rust Tokio server | 168.98us | 138.11us | 5.80ms | 91.79% | 465844.80 | 39.98MB |

## Pull Request Checks

Pull requests targeting `main` or `master` run the `CI / build-and-test`
GitHub Actions check. Configure both branches in GitHub branch protection or a
repository ruleset with:

- Require a pull request before merging
- Require status checks to pass before merging
- Required status check: `build-and-test` (shown in Actions as `CI / build-and-test`)
- Restrict direct pushes to matching branches
