#!/usr/bin/env python
"""Debug verb slots during dialog."""
import json, time
from utils import McpClient, launch_scummvm, wait_for_mcp, GAME_PATHS

mcp_port = 23469
proc = launch_scummvm("comi-demo", GAME_PATHS["comi-demo"], port=mcp_port,
    scummvm_binary="/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm")
try:
    client = wait_for_mcp("127.0.0.1", mcp_port, timeout=30.0)
    time.sleep(2)
    
    client.act("talk_to", "small_pirate")
    time.sleep(2)
    
    # First answer
    result = client.answer(6)
    time.sleep(2)
    
    # Check debug state
    debug = client.debug()
    print("=== Debug state ===")
    if "verbs" in debug:
        for v in debug["verbs"]:
            print(f"slot={v.get('slot')} verbid={v.get('verbid')} curmode={v.get('curmode')} color={v.get('color')} key={v.get('key')} label='{v.get('label')}'")
    else:
        print(json.dumps(debug, indent=2)[:1500])
    
    client.close()
finally:
    proc.kill()
    proc.wait(timeout=5)
