#!/usr/bin/env python
"""Simple launch test with longer timeout."""

import subprocess
import time
import os
import httpx

game_path = "/Users/xhardy/Personal/llm/scummvm/games/COMIDEMO"

import tempfile

tmpdir = tempfile.mkdtemp()
ini_path = os.path.join(tmpdir, "scummvm.ini")
ini_content = f"""[scummvm]
lastselectedgame=comi-demo
mute=false
debuglevel=11

[comi-demo]
description=COMI
path={game_path}
mcp_port=23469
mcp=true
mcp_skip_tool=true
mute=true
engineid=scumm
gameid=comi
language=en
copy_protection=false
"""
with open(ini_path, "w") as f:
    f.write(ini_content)

env = os.environ.copy()
env["SDL_AUDIODRIVER"] = "dummy"

args = [
    "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm",
    "-c",
    ini_path,
    "--save-slot=1",
    "--savepath=/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/test/mcp/save_slots/comi-demo",
    "comi-demo",
]

print("Launching...")
proc = subprocess.Popen(
    args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
)

# Try connecting periodically
for i in range(60):
    time.sleep(1)
    try:
        client = httpx.Client(timeout=httpx.Timeout(2.0))
        resp = client.post(
            "http://127.0.0.1:23469/mcp",
            json={
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-03-26",
                    "clientInfo": {"name": "test", "version": "1.0"},
                },
            },
        )
        print(f"Connected after {i + 1}s! Status: {resp.status_code}")
        print(f"Response: {resp.text[:300]}")
        break
    except (httpx.ConnectError, httpx.ReadTimeout, httpx.WriteTimeout) as e:
        print(f"  {i + 1}s: not yet ({type(e).__name__})")
    except Exception as e:
        print(f"  {i + 1}s: ERROR {type(e).__name__}: {e}")

proc.kill()
