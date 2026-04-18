#!/bin/bash

# Simple script to run MCP tests
echo "Running MCP unit tests..."

# Check if we have the test binary
if [ -f "test/mcp_test" ]; then
    cd test
    ./mcp_test
    cd ..
else
    echo "Test binary not found. Please build tests first."
    exit 1
fi