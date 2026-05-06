#!/usr/bin/env python
"""Test direct launch."""

import subprocess
import time
import os

game_path = "/Users/xhardy/Personal/llm/scummvm/games/COMIDEMO"
ini_content = f"""[scummvm]
lastselectedgame=comi-demo
browser_lastpath={game_path}
mute=false
debuglevel=11

[comi-demo]
description=The Curse of Monkey Island (Demo)
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

import tempfile

tmpdir = tempfile.mkdtemp()
ini_path = os.path.join(tmpdir, "scummvm.ini")
with open(ini_path, "w") as f:
    f.write(ini_content)

print(f"INI path: {ini_path}")
print(f"Game path: {game_path}")

args = [
    "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm",
    "-c",
    ini_path,
    "--save-slot=1",
    "--savepath=/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/test/mcp/save_slots/comi-demo",
    "--talkspeed=1200",
    "comi-demo",
]

print(f"Command: {' '.join(args)}")
print("Starting...")
proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

for i in range(30):
    time.sleep(1)
    print(f"Waiting... {i + 1}s")
    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        print(f"Process exited with code {proc.returncode}")
        print(f"STDOUT:\n{stdout.decode()[:500]}")
        print(f"STDERR:\n{stderr.decode()[:500]}")
        break

if proc.poll() is None:
    print("Process still running after 30s")
    proc.kill()
