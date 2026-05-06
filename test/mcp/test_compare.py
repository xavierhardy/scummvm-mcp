#!/usr/bin/env python
"""Compare look_at vs v_6."""

import json
import time
from utils import McpClient, launch_scummvm, wait_for_mcp, GAME_PATHS

mcp_port = 23469
proc = launch_scummvm(
    "comi-demo",
    GAME_PATHS["comi-demo"],
    port=mcp_port,
    scummvm_binary="/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm",
)
try:
    client = wait_for_mcp("127.0.0.1", mcp_port, timeout=30.0)
    time.sleep(2)
    
    # Test look_at first
    print("=== look_at cannon_balls ===")
    result = client.act("look_at", "cannon_balls")
    print(f"Result: {json.dumps(result, indent=2)}")
    
    # Wait
    time.sleep(2)
    
    # Test v_6
    print("\n=== v_6 cannon_balls ===")
    result = client.act("v_6", "cannon_balls")
    print(f"Result: {json.dumps(result, indent=2)}")
    
    client.close()
finally:
    proc.kill()
    proc.wait(timeout=5)
