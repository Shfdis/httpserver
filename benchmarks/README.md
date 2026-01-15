# HTTP Server Benchmark Suite

This directory contains benchmarking tools to compare the C++ io_uring HTTP server with a Rust Tokio HTTP server.

## Prerequisites

### Required Tools

1. **wrk** - HTTP benchmarking tool
   - Arch: `sudo pacman -S wrk`
   - Ubuntu/Debian: `sudo apt-get install wrk`
   - macOS: `brew install wrk`

2. **Rust** - For building the Tokio server
   - Install from [rustup.rs](https://rustup.rs/)

3. **C++ Build Tools** - For building the C++ server
   - CMake
   - liburing
   - C++20 compiler

## Building

### C++ Server

```bash
cd ../example
mkdir -p build && cd build
cmake ..
make
cd ../..
```

### Rust Tokio Server

```bash
cd rust_server
cargo build --release
cd ..
```

## Running Benchmarks

Run the benchmark script from the `benchmarks` directory:

```bash
chmod +x benchmark.sh
./benchmark.sh
```

The script will:
1. Check if both servers are built
2. Start both servers (C++ on port 8080, Rust on port 8081)
3. Run benchmarks for GET and POST requests
4. Generate a comparison report
5. Stop both servers

## Benchmark Configuration

You can modify the benchmark parameters in `benchmark.sh`:

- `DURATION`: How long to run each test (default: 30s)
- `THREADS`: Number of threads (default: 4)
- `CONNECTIONS`: Number of concurrent connections (default: 100)

Example:
```bash
DURATION=60s THREADS=8 CONNECTIONS=200 ./benchmark.sh
```

## Results

Results are saved in the `results/` directory with timestamps:
- Individual benchmark results: `cpp_TIMESTAMP.txt`, `rust_TIMESTAMP.txt`
- Comparison report: `compare_TIMESTAMP.txt`

## Manual Testing

You can also test the servers manually:

### C++ Server
```bash
cd ../example
./echo_server
# Server runs on port 8080
```

### Rust Tokio Server
```bash
cd rust_server
cargo run --release
# Server runs on port 8081
```

### Test Requests

```bash
# GET request
curl "http://127.0.0.1:8080/echo?msg=hello"

# POST request
echo "hello world" | curl -X POST -d @- http://127.0.0.1:8080/echo
```

## Understanding Results

Key metrics to compare:

- **Requests/sec**: Higher is better - throughput of the server
- **Latency**: Lower is better - response time distribution
  - Avg, Stdev, Max, +/- Stdev
- **Transfer/sec**: Higher is better - data transfer rate

## Notes

- Both servers implement the same `/echo` endpoint
- C++ server uses io_uring for async I/O
- Rust server uses Tokio runtime
- Benchmarks run sequentially to avoid resource contention