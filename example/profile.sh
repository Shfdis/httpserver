#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FLAMEGRAPH_DIR="/tmp/FlameGraph"
OUTPUT="${SCRIPT_DIR}/flamegraph.svg"

# Configuration
DURATION=${DURATION:-8}
CONNECTIONS=${CONNECTIONS:-100}
THREADS=${THREADS:-4}
FREQ=${FREQ:-99}
URL=${URL:-"http://localhost:8080/echo?msg=hello"}

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[+]${NC} $1"; }
warn() { echo -e "${YELLOW}[!]${NC} $1"; }
err() { echo -e "${RED}[x]${NC} $1"; exit 1; }

cleanup() {
    log "Cleaning up..."
    pkill -f "echo_server" 2>/dev/null || true
    rm -f "${SCRIPT_DIR}/perf.data"
}
trap cleanup EXIT

# Check dependencies
command -v perf >/dev/null || err "perf not found. Install linux-tools or perf."
command -v wrk >/dev/null || err "wrk not found. Install wrk."

# Download FlameGraph tools if needed
if [[ ! -d "$FLAMEGRAPH_DIR" ]]; then
    log "Downloading FlameGraph tools..."
    git clone --depth 1 https://github.com/brendangregg/FlameGraph.git "$FLAMEGRAPH_DIR"
fi

# Build if needed
if [[ ! -x "${SCRIPT_DIR}/echo_server" ]]; then
    log "Building echo_server..."
    cd "$SCRIPT_DIR" && make
fi

# Kill any existing server
pkill -f "echo_server" 2>/dev/null || true
sleep 0.5

# Start the server
log "Starting echo_server..."
cd "$SCRIPT_DIR"
./echo_server &
SERVER_PID=$!
sleep 1

if ! kill -0 $SERVER_PID 2>/dev/null; then
    err "Failed to start echo_server"
fi
log "Server running (PID: $SERVER_PID)"

# Start profiling
log "Profiling for ${DURATION}s (freq=${FREQ}Hz)..."
perf record -F "$FREQ" -g -p "$SERVER_PID" -o perf.data -- sleep $((DURATION + 2)) &
PERF_PID=$!
sleep 1

# Generate load
log "Generating load with wrk (${THREADS} threads, ${CONNECTIONS} connections)..."
wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" "$URL"

# Wait for perf to finish
wait $PERF_PID 2>/dev/null || true

# Generate flamegraph
log "Generating flamegraph..."
perf script -i perf.data 2>/dev/null \
    | "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" \
    | "$FLAMEGRAPH_DIR/flamegraph.pl" \
        --title "echo_server CPU Flamegraph" \
        --width 1400 \
    > "$OUTPUT"

log "Flamegraph saved to: ${OUTPUT}"
echo ""
echo "View with:  xdg-open ${OUTPUT}"
echo "Or:         firefox ${OUTPUT}"
