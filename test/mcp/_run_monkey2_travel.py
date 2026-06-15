"""MI2 demo: clear the Largo dialog, travel the Scabb map, find the demo end."""

import os
import subprocess
import sys
import tempfile
import time

from utils import MCP_HOST, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/monkey2-dos-ni-demo-en"
PORT = 23485
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


def names(c):
    return [o.get("name") for o in stt(c).get("objects", [])]


def clear_dialog(c, tag=""):
    """Answer/skip until no question persists across two polls."""
    msgs = []
    quiet = 0
    for _ in range(25):
        q = stt(c).get("question")
        if not isinstance(q, dict):
            quiet += 1
            if quiet >= 2:
                break
            time.sleep(0.8)
            continue
        quiet = 0
        ch = q.get("choices", [])
        cid = ch[-1]["id"] if ch else 1
        r = safe(c.answer, cid)
        if "_err" in r:
            safe(c.skip)
            time.sleep(1.2)
        else:
            msgs += txt(r)
    if msgs and tag:
        print(f"   [{tag}]", " | ".join(msgs[-8:]), flush=True)
    return msgs


def travel(c, place):
    """Travel to a map location by name; return new room or None."""
    before = rid(c)
    clear_dialog(c, "")
    r = safe(c.act, "walk_to", place)
    time.sleep(2.5)
    clear_dialog(c, "")
    after = rid(c)
    return after if after != before else None


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

        safe(c.act, "walk_to", "path")
        time.sleep(1.5)
        clear_dialog(c, "largo")
        time.sleep(2.0)
        print("AFTER LARGO room", rid(c), "inv", stt(c).get("inventory"), flush=True)

        # Cross to the map and make sure the dialog is fully cleared.
        safe(c.act, "walk_to", "path")
        time.sleep(2.0)
        clear_dialog(c, "post-cross")
        print("MAP room", rid(c), "objs", names(c), flush=True)

        # Visit each Scabb location; dump rooms/objects; watch for a demo end.
        for place in ("woodtick", "swamp", "cemetery", "beach", "house"):
            r = travel(c, place)
            print(f"\n=== travel -> {place}: room {r} ===", flush=True)
            if r is None:
                continue
            # explore this location a little
            for _ in range(4):
                s = stt(c)
                ns = [o.get("name") for o in s.get("objects", [])]
                print(f"   room {(s.get('room') or {}).get('id')} objs {ns}", flush=True)
                spoke = False
                for n in ("bartender", "barkeep", "barkeeper", "voodoo", "fortune",
                          "wally", "man", "pirate"):
                    if n in ns:
                        rr = safe(c.act, "talk_to", n)
                        print(f"     talk {n}: {txt(rr)}", flush=True)
                        clear_dialog(c, f"{place}-talk")
                        spoke = True
                        break
                if not spoke:
                    # step deeper through an interior exit if present
                    for n in ("door", "ship", "hatch", "entrance", "path"):
                        if n in ns:
                            safe(c.act, "walk_to", n)
                            time.sleep(1.8)
                            clear_dialog(c, "")
                            break
                time.sleep(0.6)
            # back to the map for the next location
            safe(c.act, "walk_to", "map")
            time.sleep(1.5)
            if rid(c) != 2:
                # many MI2 rooms have a 'map'/'island' exit; otherwise try a door
                for n in ("island", "exit", "ship", "door"):
                    if n in names(c):
                        safe(c.act, "walk_to", n)
                        time.sleep(1.8)
                        if rid(c) == 2:
                            break
            print("   back at room", rid(c), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
