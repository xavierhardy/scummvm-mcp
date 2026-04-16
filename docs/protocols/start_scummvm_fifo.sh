#!/bin/sh
# Helper to start ScummVM attached to two FIFOs for MCP stdio-file transport.
# Usage: ./start_scummvm_fifo.sh /tmp/mcp-in.fifo /tmp/mcp-out.fifo /path/to/scummvm /path/to/scummvm.ini

set -e
IN_FIFO=${1:-/tmp/mcp-in.fifo}
OUT_FIFO=${2:-/tmp/mcp-out.fifo}
SCUMMVM_BIN=${3:-/usr/local/bin/scummvm}
SCUMMVM_INI=${4:-./scummvm.ini}

# Create FIFOs if they don't exist
if [ ! -p "$IN_FIFO" ]; then
    rm -f "$IN_FIFO"
    mkfifo "$IN_FIFO"
fi
if [ ! -p "$OUT_FIFO" ]; then
    rm -f "$OUT_FIFO"
    mkfifo "$OUT_FIFO"
fi

# Start scummvm with stdin from IN_FIFO and stdout to OUT_FIFO
# Important: the FIFOs must be opened by both ends. Start scummvm in background, then the client
# should open the FIFOs from the other side (or use tail/head) to avoid blocking.

echo "Starting ScummVM with stdin <$IN_FIFO and stdout >$OUT_FIFO"
"$SCUMMVM_BIN" -c "$SCUMMVM_INI" --debugflags monkey_mcp,scumm --debuglevel 11 < "$IN_FIFO" > "$OUT_FIFO" &
SCUMMVM_PID=$!

echo "ScummVM started (pid $SCUMMVM_PID)."
echo "Client should write JSON-RPC requests to: $IN_FIFO"
echo "Client should read JSON-RPC responses from: $OUT_FIFO"

echo "Press Ctrl-C to stop."

trap "echo Stopping scummvm; kill $SCUMMVM_PID; exit 0" INT TERM
wait $SCUMMVM_PID
