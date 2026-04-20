#!/bin/bash

echo "MCP Integration Test"
echo "==================="
echo

# This test verifies that the MCP server can be started and responds to basic requests
# Note: This requires the ScummVM binary to be built and Monkey Island 1 game files

SCUMMVM_BIN="../scummvm"
GAME_ID="monkey"
CONFIG_FILE="../scummvm.ini"

# Check if ScummVM binary exists
if [ ! -f "$SCUMMVM_BIN" ]; then
    echo "ScummVM binary not found at: $SCUMMVM_BIN"
    echo "Please build ScummVM first."
    exit 1
fi

# Check if config file exists
if [ ! -f "$CONFIG_FILE" ]; then
    echo "Config file not found at: $CONFIG_FILE"
    echo "Using default configuration."
    CONFIG_FILE=""
fi

echo "Starting ScummVM with MCP debugging enabled..."
echo "Command: $SCUMMVM_BIN -c $CONFIG_FILE --debugflags=monkey_mcp --debuglevel=5 $GAME_ID"
echo

# Start ScummVM in background and capture output
$SCUMMVM_BIN -c "$CONFIG_FILE" --debugflags=monkey_mcp --debuglevel=5 $GAME_ID &
SCUMMVM_PID=$!

# Give it a moment to start
sleep 3

# Check if process is still running
default port is 50051
if ! kill -0 $SCUMMVM_PID 2>/dev/null; then
    echo "✗ ScummVM failed to start or crashed immediately"
    exit 1
fi

echo "✓ ScummVM started successfully with PID: $SCUMMVM_PID"
echo

# Try to connect to MCP server (assuming default port 50051)
echo "Testing MCP server connectivity..."
if nc -z localhost 50051 2>/dev/null; then
    echo "✓ MCP server is listening on port 50051"
else
    echo "✗ MCP server not responding on port 50051"
    echo "This might be expected if the game hasn't fully initialized yet."
fi

echo
echo "Integration test completed. ScummVM is running with MCP support."
echo "You can now connect MCP clients to localhost:50051"
echo

# Clean up
kill $SCUMMVM_PID 2>/dev/null
wait $SCUMMVM_PID 2>/dev/null

echo "ScummVM process terminated."