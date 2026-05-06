#!/usr/bin/env python
"""Test different verb IDs to find the correct CMI verbs."""

import json
import time
from utils import McpClient, launch_scummvm, wait_for_mcp, GAME_PATHS

mcp_port = 23469
scummvm_binary = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"

# Use the debug tool to send raw verb IDs
# We need to add a way to call doSentence directly with a verb ID
# Let's test by calling act with int verbids directly

def test():
    proc = launch_scummvm(
        "comi-demo",
        GAME_PATHS["comi-demo"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
    )
    try:
        client = wait_for_mcp("127.0.0.1", mcp_port, timeout=30.0)
        time.sleep(2)
        
        state = client.state()
        print(f"Initial state - position: {state.get('position')}")
        
        # Get small_pirate id
        pirate_id = None
        for obj in state["objects"]:
            if obj["name"] == "small_pirate":
                pirate_id = obj["id"]
                break
        
        ramrod_id = None
        for obj in state["objects"]:
            if obj["name"] == "ramrod":
                ramrod_id = obj["id"]
                break
        
        print(f"small_pirate id: {pirate_id}, ramrod id: {ramrod_id}")
        
        client.close()
    finally:
        proc.kill()
        proc.wait(timeout=5)

test()
