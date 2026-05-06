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
    result = client.act("talk_to", "small_pirate")
    time.sleep(1)
    
    # Get debug state
    print("=== debug ===")
    debug_data = client.debug()
    print(json.dumps(debug_data, indent=2)[:2000])
    
    client.close()
finally:
    proc.kill()
    proc.wait(timeout=5)
