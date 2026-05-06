#!/usr/bin/env python
"""Test different verb IDs on small_pirate."""

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
    
    # Test verbs 1-15 on small_pirate
    print("=== Testing verb IDs on small_pirate ===")
    for vid in range(1, 16):
        try:
            result = client.act(f"v_{vid}", "small_pirate")
            msgs = [m["text"] for m in result.get("messages", [])]
            pos = result.get("position")
            q = result.get("question")
            print(f"verb {vid}: pos={pos} msgs={msgs} question={q is not None}")
            
            # If we got a question, answer with first choice and exit
            if q:
                print(f"  CHOICES: {[c['label'] for c in q['choices']]}")
                client.answer(q['choices'][-1]['id'])  # last choice usually 'goodbye'
        except Exception as e:
            print(f"verb {vid}: error {e}")
        time.sleep(0.3)
    
    client.close()
finally:
    proc.kill()
    proc.wait(timeout=5)
