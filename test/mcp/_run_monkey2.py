"""Drive the Monkey Island 2 rolling demo: skip intro -> Woodtick -> explore.

Goal: discover the real room ids / object names / dialogue and the demo's end so
the best-effort monkey2_demo.py goal set can be reconciled.
"""

import os
import subprocess
import sys
import tempfile
import time

from utils import MCP_HOST, McpClient, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/monkey2-dos-ni-demo-en"
PORT = 23480
TARGET = "monkey2-demo"


def launch():
    tmpl = open(os.path.join("ini_files", f"scummvm_{TARGET}.ini")).read()
    logfile = os.path.join("logs", f"scummvm_{TARGET}_{PORT}.scummvm.log")
    os.makedirs("logs", exist_ok=True)
    content = tmpl % {"game_path": GAME, "mcp_port": PORT, "logfile": logfile}
    tmpdir = tempfile.mkdtemp(prefix=f"run_{TARGET}_")
    ini_path = os.path.join(tmpdir, "scummvm.ini")
    with open(ini_path, "w") as f:
        f.write(content)
    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = "dummy"
    out = open(os.path.join("logs", f"scummvm_{TARGET}_{PORT}.out"), "w")
    proc = subprocess.Popen(
        [BIN, "-c", ini_path, TARGET], env=env, stdout=out, stderr=subprocess.STDOUT
    )
    return proc


def texts(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def dump(c, label):
    s = c.state()
    room = (s.get("room") or {}).get("id")
    q = s.get("question")
    print(f"[{label}] room={room} inv={s.get('inventory')} verbs={s.get('verbs')}", flush=True)
    print("   objects:", [o.get("name") for o in s.get("objects", [])], flush=True)
    if q:
        print("   question:", [ch.get("label") for ch in q.get("choices", [])], flush=True)
    return s


def main():
    proc = launch()
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        # Skip the rolling intro until we have control (stable room with verbs).
        last = None
        for i in range(30):
            try:
                c.skip()
            except Exception:
                pass
            time.sleep(1.0)
            try:
                s = c.state()
            except Exception:
                continue
            room = (s.get("room") or {}).get("id")
            verbs = s.get("verbs")
            if verbs and room == last and room is not None:
                print(f"  control at skip #{i}, room={room}", flush=True)
                break
            last = room
        dump(c, "after-skip")
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
