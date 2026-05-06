#!/usr/bin/env python
import json
import httpx
import time

# Try to connect to the MCP server
try:
    client = httpx.Client(timeout=httpx.Timeout(10.0))

    payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {
            "protocolVersion": "2025-03-26",
            "clientInfo": {"name": "test_client", "version": "1.0"},
        },
    }

    print("Sending initialize request...")
    resp = client.post(
        "http://127.0.0.1:23469/mcp",
        json=payload,
        headers={"Content-Type": "application/json"},
    )
    print(f"Status: {resp.status_code}")
    print(f"Response: {resp.text[:500]}")

    # Try state() call
    payload2 = {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "tools/call",
        "params": {"name": "state", "arguments": {}},
    }

    print("\nSending state request...")
    resp2 = client.post(
        "http://127.0.0.1:23469/mcp",
        json=payload2,
        headers={
            "Content-Type": "application/json",
            "Mcp-Session-Id": resp.headers.get("Mcp-Session-Id", ""),
        },
    )
    print(f"Status: {resp2.status_code}")
    print(f"Response length: {len(resp2.text)}")
    print(f"Response (first 500 chars): {resp2.text[:500]}")

except Exception as e:
    print(f"Error: {e}")
