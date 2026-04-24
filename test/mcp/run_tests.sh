#!/bin/bash
# Run MCP integration tests with pytest

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Determine Python executable
if [ -z "$PYTHON" ]; then
    if command -v python3 &>/dev/null; then
        PYTHON=python3
    elif command -v python &>/dev/null; then
        PYTHON=python
    else
        echo "Error: python3 not found. Set PYTHON environment variable." >&2
        exit 1
    fi
fi

# Check if pytest is installed
if ! "$PYTHON" -m pytest --version &>/dev/null; then
    echo "Error: pytest not installed. Run: $PYTHON -m pip install pytest httpx" >&2
    exit 1
fi

# Run tests
"$PYTHON" -m pytest "$@"
