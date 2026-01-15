#!/bin/bash

# Quick test script to verify both servers work

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "Testing C++ server on port 8080..."
CPP_SERVER=""
if [ -f "../example/echo_server" ]; then
    CPP_SERVER="../example/echo_server"
elif [ -f "../example/build/echo_server" ]; then
    CPP_SERVER="../example/build/echo_server"
fi

if [ -z "$CPP_SERVER" ]; then
    echo -e "${RED}C++ server binary not found${NC}"
else
    cd "$(dirname "$CPP_SERVER")"
    ./$(basename "$CPP_SERVER") &
    CPP_PID=$!
    cd "$SCRIPT_DIR"
    sleep 2
    
    if curl -s "http://127.0.0.1:8080/echo?msg=test" | grep -q "test"; then
        echo -e "${GREEN}✓ C++ GET works${NC}"
    else
        echo -e "${RED}✗ C++ GET failed${NC}"
    fi
    
    if echo "posttest" | curl -s -X POST -d @- "http://127.0.0.1:8080/echo" | grep -q "posttest"; then
        echo -e "${GREEN}✓ C++ POST works${NC}"
    else
        echo -e "${RED}✗ C++ POST failed${NC}"
    fi
    
    kill $CPP_PID 2>/dev/null || true
fi

echo ""
echo "Testing Rust Tokio server on port 8081..."
cd rust_server
if [ ! -f "target/release/tokio-echo-server" ]; then
    echo -e "${YELLOW}Building Rust server...${NC}"
    cargo build --release
fi

cargo run --release &
RUST_PID=$!
cd "$SCRIPT_DIR"
sleep 2

if curl -s "http://127.0.0.1:8081/echo?msg=test" | grep -q "test"; then
    echo -e "${GREEN}✓ Rust GET works${NC}"
else
    echo -e "${RED}✗ Rust GET failed${NC}"
fi

if echo "posttest" | curl -s -X POST -d @- "http://127.0.0.1:8081/echo" | grep -q "posttest"; then
    echo -e "${GREEN}✓ Rust POST works${NC}"
else
    echo -e "${RED}✗ Rust POST failed${NC}"
fi

kill $RUST_PID 2>/dev/null || true

echo ""
echo -e "${GREEN}Quick test completed!${NC}"