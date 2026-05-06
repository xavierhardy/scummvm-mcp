#!/usr/bin/env python
import json, time
from utils import McpClient, launch_scummvm, wait_for_mcp, GAME_PATHS

mcp_port = 23469
proc = launch_scummvm("comi-demo", GAME_PATHS["comi-demo"], port=mcp_port,
    scummvm_binary="/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm")
try:
    client = wait_for_mcp("127.0.0.1", mcp_port, timeout=30.0)
    time.sleep(2)
    
    # Talk to pirate
    print("=== talk_to small_pirate ===")
    result = client.act("talk_to", "small_pirate")
    print(json.dumps(result, indent=2))
    
    time.sleep(2)
    
    # Answer 6
    print("\n=== answer(6) ===")
    result = client.answer(6)
    print(json.dumps(result, indent=2))
    
    time.sleep(2)
    
    # Check state - should there be a new dialog?
    print("\n=== state after answer ===")
    state = client.state()
    if "question" in state:
        print(f"Question with {len(state['question'].get('choices',[]))} choices")
    print(f"Inventory: {state.get('inventory')}")
    
    client.close()
finally:
    proc.kill()
    proc.wait(timeout=5)
