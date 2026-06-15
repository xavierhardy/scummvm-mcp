"""Explore the MI2 demo (Woodtick) to map rooms/objects and find the demo end."""

import os
import subprocess
import sys
import tempfile
import time

from utils import MCP_HOST, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/monkey2-dos-ni-demo-en"
PORT = 23481
TARGET = "monkey2-demo"


def launch():
    tmpl = open(os.path.join("ini_files", f"scummvm_{TARGET}.ini")).read()
    logfile = os.path.join("logs", f"scummvm_{TARGET}_{PORT}.scummvm.log")
    content = tmpl % {"game_path": GAME, "mcp_port": PORT, "logfile": logfile}
    tmpdir = tempfile.mkdtemp(prefix=f"run_{TARGET}_")
    ini_path = os.path.join(tmpdir, "scummvm.ini")
    open(ini_path, "w").write(content)
    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = "dummy"
    out = open(os.path.join("logs", f"scummvm_{TARGET}_{PORT}.out"), "w")
    return subprocess.Popen([BIN, "-c", ini_path, TARGET], env=env, stdout=out, stderr=subprocess.STDOUT)


def texts(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def rid(c):
    return (c.state().get("room") or {}).get("id")


def detail(c, label):
    s = c.state()
    print(f"\n[{label}] room={(s.get('room') or {}).get('id')} inv={s.get('inventory')}", flush=True)
    for o in s.get("objects", []):
        p = o.get("position") or {}
        print(f"    {o.get('name')!r:24} @({p.get('x')},{p.get('y')}) pathway={o.get('pathway')} verbs={o.get('compatible_verbs')}", flush=True)
    return s


def skip_to_control(c):
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
        if s.get("verbs") and room == last and room is not None:
            return room
        last = room
    return last


def main():
    proc = launch()
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        room = skip_to_control(c)
        print("CONTROL room", room, flush=True)
        detail(c, "woodtick-entry")

        # Pick up the "No Trezer Huntin zone" sign -> shovel.
        r = c.act("pick_up", "sign")
        print("pick_up sign ->", r.get("inventory_added"), texts(r), flush=True)

        # Walk the 'path' deeper into Woodtick (expect the Largo bridge encounter).
        for name in ("path",):
            r = c.act("walk_to", name)
            time.sleep(2.0)
            print(f"walk_to {name} -> room {rid(c)} msgs {texts(r)}", flush=True)
        detail(c, "after-path")

        # Map the ships/doors (Woodtick buildings) by walking to each pathway.
        s = c.state()
        seen = {rid(c)}
        for o in list(s.get("objects", [])):
            if not (o.get("pathway") or o.get("name") in ("door", "ship", "hatch")):
                continue
            name = o.get("name")
            before = rid(c)
            r = c.act("walk_to", name)
            time.sleep(1.8)
            after = rid(c)
            if after != before:
                print(f"  {name} -> room {after} msgs {texts(r)}", flush=True)
                if after not in seen:
                    seen.add(after)
                    detail(c, f"room-{after}")
        print("\nSEEN ROOMS:", sorted(x for x in seen if x is not None), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
