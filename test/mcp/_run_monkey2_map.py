"""MI2 demo: skip -> Largo robs you -> Scabb Island map -> visit locations.

Map layout (per observation): Woodtick top, Swamp right, Cemetery bottom.
Find how travel works and where the demo ends.
"""

import os
import subprocess
import sys
import tempfile
import time

from utils import MCP_HOST, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/monkey2-dos-ni-demo-en"
PORT = 23484
TARGET = "monkey2-demo"


def launch():
    tmpl = open(os.path.join("ini_files", f"scummvm_{TARGET}.ini")).read()
    logfile = os.path.join("logs", f"scummvm_{TARGET}_{PORT}.scummvm.log")
    content = tmpl % {"game_path": GAME, "mcp_port": PORT, "logfile": logfile}
    ini = os.path.join(tempfile.mkdtemp(prefix=f"run_{TARGET}_"), "scummvm.ini")
    open(ini, "w").write(content)
    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = "dummy"
    out = open(os.path.join("logs", f"scummvm_{TARGET}_{PORT}.out"), "w")
    return subprocess.Popen([BIN, "-c", ini, TARGET], env=env, stdout=out, stderr=subprocess.STDOUT)


def safe(fn, *a):
    try:
        return fn(*a)
    except Exception as e:
        return {"_err": str(e)}


def stt(c):
    s = safe(c.state)
    return s if isinstance(s, dict) else {}


def rid(c):
    return (stt(c).get("room") or {}).get("id")


def txt(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def detail(c, label):
    s = stt(c)
    print(f"[{label}] room={(s.get('room') or {}).get('id')} inv={s.get('inventory')}", flush=True)
    print("   objs:", [o.get("name") for o in s.get("objects", [])], flush=True)
    q = s.get("question")
    if isinstance(q, dict):
        print("   Q:", [x.get("label") for x in q.get("choices", [])], flush=True)
    return s


def drain(c, tag, pick_id=2):
    out = []
    for _ in range(8):
        q = stt(c).get("question")
        if not isinstance(q, dict):
            break
        ch = q.get("choices", [])
        cid = pick_id if any(x.get("id") == pick_id for x in ch) else (ch[-1]["id"] if ch else 1)
        r = safe(c.answer, cid)
        if "_err" in r:
            safe(c.skip)
            time.sleep(1.2)
            continue
        out += txt(r)
        pick_id = ch[-1]["id"] if ch else 1  # after first, just keep progressing
    if out:
        print(f"   [{tag}]", " | ".join(out), flush=True)
    return out


def main():
    proc = launch()
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        last = None
        for _ in range(30):
            safe(c.skip)
            time.sleep(1.0)
            s = stt(c)
            r = (s.get("room") or {}).get("id")
            if s.get("verbs") and r == last and r is not None:
                break
            last = r
        print("CONTROL room", rid(c), flush=True)

        # Largo: take-my-money to streamline, then he robs you.
        safe(c.act, "walk_to", "path")
        time.sleep(1.5)
        drain(c, "largo", pick_id=2)
        time.sleep(2.5)
        print("AFTER LARGO room", rid(c), "inv", stt(c).get("inventory"), flush=True)

        # Cross into the island map.
        safe(c.act, "walk_to", "path")
        time.sleep(2.0)
        m = detail(c, "MAP?")
        map_room = (m.get("room") or {}).get("id")

        # Discover travel: dump objects; try walk_to each name; else click regions.
        names = [o.get("name") for o in m.get("objects", [])]
        print("MAP object names:", names, flush=True)
        # Try to travel to Woodtick by name, then by clicking the top of the map.
        for attempt in ("woodtick", "town"):
            if attempt in names:
                safe(c.act, "walk_to", attempt)
                time.sleep(2.0)
                break
        if rid(c) == map_room:
            # top-center click (Woodtick)
            for (x, y) in ((160, 40), (160, 60), (200, 50)):
                safe(c.walk, x, y)
                time.sleep(2.0)
                if rid(c) != map_room:
                    break
        detail(c, "after-travel-woodtick")
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
