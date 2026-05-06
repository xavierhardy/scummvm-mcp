#!/usr/bin/env python
"""Test verb 8 (suspected talk_to) on small_pirate."""

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
    
    # First walk to small_pirate
    print("=== walk_to small_pirate ===")
    result = client.act("v_13", "small_pirate")
    print(json.dumps(result, indent=2))
    time.sleep(1)
    
    # Now try verb 8
    print("\n=== verb 8 small_pirate ===")
    result = client.act("v_8", "small_pirate")
    print(json.dumps(result, indent=2))
    time.sleep(1)
    
    # Check state
    state = client.state()
    print("\n=== STATE ===")
    if "question" in state:
        print(f"Question: {json.dumps(state['question'], indent=2)}")
    else:
        print("No question in state")
    
    client.close()
finally:
    proc.kill()
    proc.wait(timeout=5)
