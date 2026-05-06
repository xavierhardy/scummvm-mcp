#!/usr/bin/env python
"""Test different verb IDs systematically."""

import json
import time
from utils import McpClient, launch_scummvm, wait_for_mcp, GAME_PATHS

mcp_port = 23469
scummvm_binary = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"

proc = launch_scummvm(
    "comi-demo",
    GAME_PATHS["comi-demo"],
    port=mcp_port,
    scummvm_binary=scummvm_binary,
)
try:
    client = wait_for_mcp("127.0.0.1", mcp_port, timeout=30.0)
    time.sleep(2)
    
    # Test verbs 1-15 on cannon_balls
    print("=== Testing verb IDs on cannon_balls ===")
    for vid in range(1, 16):
        try:
            result = client.act(f"v_{vid}", "cannon_balls")
            msgs = [m["text"] for m in result.get("messages", [])]
            pos = result.get("position")
            if msgs or pos:
                print(f"verb {vid}: pos={pos} msgs={msgs}")
            else:
                print(f"verb {vid}: empty")
        except Exception as e:
            print(f"verb {vid}: error {e}")
        time.sleep(0.3)
    
    client.close()
finally:
    proc.kill()
    proc.wait(timeout=5)
