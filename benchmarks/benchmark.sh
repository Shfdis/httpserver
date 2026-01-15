#!/bin/bash

set -e

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if wrk is installed
if ! command -v wrk &> /dev/null; then
    echo -e "${YELLOW}wrk is not installed. Installing dependencies...${NC}"
    echo "Please install wrk:"
    echo "  Arch: sudo pacman -S wrk"
    echo "  Ubuntu/Debian: sudo apt-get install wrk"
    echo "  macOS: brew install wrk"
    exit 1
fi

# Configuration
DURATION=30s
THREADS=4
CONNECTIONS=100
CPP_PORT=8080
RUST_PORT=8081
RESULTS_DIR="results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$RESULTS_DIR"

echo -e "${BLUE}=== HTTP Server Benchmark Suite ===${NC}\n"

# Function to check if server is running
check_server() {
    local port=$1
    local name=$2
    for i in {1..30}; do
        if curl -s "http://127.0.0.1:$port/echo?msg=test" > /dev/null 2>&1; then
            echo -e "${GREEN}✓${NC} $name server is ready"
            return 0
        fi
        sleep 1
    done
    echo -e "${RED}✗${NC} $name server failed to start"
    return 1
}

# Function to run benchmark
run_benchmark() {
    local name=$1
    local port=$2
    local endpoint=$3
    local method=$4
    local output_file="$RESULTS_DIR/${name}_${TIMESTAMP}.txt"
    
    echo -e "\n${BLUE}Benchmarking $name server...${NC}"
    echo "  Endpoint: $method $endpoint"
    echo "  Duration: $DURATION"
    echo "  Threads: $THREADS"
    echo "  Connections: $CONNECTIONS"
    
    if [ "$method" = "GET" ]; then
        wrk -t$THREADS -c$CONNECTIONS -d$DURATION --latency "http://127.0.0.1:$port$endpoint?msg=benchmark" > "$output_file" 2>&1
    else
        # Create POST script for wrk
        POST_SCRIPT=$(mktemp)
        cat > "$POST_SCRIPT" <<'WRKSCRIPT'
wrk.method = "POST"
wrk.body   = "benchmark data"
wrk.headers["Content-Type"] = "text/plain"
WRKSCRIPT
        wrk -t$THREADS -c$CONNECTIONS -d$DURATION --latency -s "$POST_SCRIPT" "http://127.0.0.1:$port$endpoint" > "$output_file" 2>&1
        rm -f "$POST_SCRIPT"
    fi
    
    echo -e "${GREEN}Results saved to: $output_file${NC}"
    cat "$output_file"
}

# Test scenarios
test_scenarios=(
    "GET:/echo?msg=test:GET"
    "POST:/echo:POST"
)

echo -e "${YELLOW}Starting C++ server...${NC}"
# Try to find the echo_server binary
CPP_SERVER=""
if [ -f "../example/echo_server" ]; then
    CPP_SERVER="../example/echo_server"
elif [ -f "../example/build/echo_server" ]; then
    CPP_SERVER="../example/build/echo_server"
else
    echo -e "${RED}Error: Could not find echo_server binary${NC}"
    echo "Please build it first: cd ../example && cmake -B build && cmake --build build"
    exit 1
fi

cd "$(dirname "$CPP_SERVER")"
./$(basename "$CPP_SERVER") &
CPP_PID=$!
cd "$SCRIPT_DIR"

sleep 2
if ! check_server $CPP_PORT "C++"; then
    kill $CPP_PID 2>/dev/null || true
    exit 1
fi

echo -e "\n${YELLOW}Starting Rust Tokio server...${NC}"
cd "$(dirname "$0")/rust_server"
if [ ! -d "target" ]; then
    echo "Building Rust server..."
    cargo build --release
fi
cargo run --release &
RUST_PID=$!
cd - > /dev/null

sleep 2
if ! check_server $RUST_PORT "Rust Tokio"; then
    kill $CPP_PID $RUST_PID 2>/dev/null || true
    exit 1
fi

# Run benchmarks
for scenario in "${test_scenarios[@]}"; do
    IFS=':' read -r name endpoint method <<< "$scenario"
    
    echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${YELLOW}Scenario: $method $endpoint${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    
    run_benchmark "cpp" $CPP_PORT "$endpoint" "$method"
    sleep 2
    run_benchmark "rust" $RUST_PORT "$endpoint" "$method"
    sleep 2
done

# Generate comparison report
echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Generating comparison report...${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"

COMPARE_FILE="$RESULTS_DIR/compare_${TIMESTAMP}.txt"
cat > "$COMPARE_FILE" <<EOF
HTTP Server Benchmark Comparison
Generated: $(date)
Test Duration: $DURATION
Threads: $THREADS
Connections: $CONNECTIONS

C++ Server (io_uring): Port $CPP_PORT
Rust Tokio Server: Port $RUST_PORT

Results Summary:
EOF

for scenario in "${test_scenarios[@]}"; do
    IFS=':' read -r name endpoint method <<< "$scenario"
    echo "" >> "$COMPARE_FILE"
    echo "=== $method $endpoint ===" >> "$COMPARE_FILE"
    
    cpp_file=$(ls -t "$RESULTS_DIR"/cpp_*_${TIMESTAMP}.txt 2>/dev/null | head -1)
    rust_file=$(ls -t "$RESULTS_DIR"/rust_*_${TIMESTAMP}.txt 2>/dev/null | head -1)
    
    if [ -f "$cpp_file" ]; then
        echo -e "\n[C++ Server]" >> "$COMPARE_FILE"
        grep -E "(Requests/sec|Latency|Transfer/sec)" "$cpp_file" >> "$COMPARE_FILE" || echo "Results not found" >> "$COMPARE_FILE"
    fi
    
    if [ -f "$rust_file" ]; then
        echo -e "\n[Rust Tokio Server]" >> "$COMPARE_FILE"
        grep -E "(Requests/sec|Latency|Transfer/sec)" "$rust_file" >> "$COMPARE_FILE" || echo "Results not found" >> "$COMPARE_FILE"
    fi
done

cat "$COMPARE_FILE"
echo -e "\n${GREEN}Full comparison saved to: $COMPARE_FILE${NC}"

# Cleanup
echo -e "\n${YELLOW}Stopping servers...${NC}"
kill $CPP_PID $RUST_PID 2>/dev/null || true
wait $CPP_PID $RUST_PID 2>/dev/null || true

echo -e "${GREEN}Benchmark completed!${NC}"