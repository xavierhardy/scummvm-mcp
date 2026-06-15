"""Play the MI2 demo through the Largo bridge encounter into Woodtick, logging
every message/room so the demo's arc and end can be identified."""

import os
import subprocess
import sys
import tempfile
import time

from utils import MCP_HOST, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/monkey2-dos-ni-demo-en"
PORT = 23482
TARGET = "monkey2-demo"


def launch():
    tmpl = open(os.path.join("ini_files", f"scummvm_{TARGET}.ini")).read()
    logfile = os.path.join("logs", f"scummvm_{TARGET}_{PORT}.scummvm.log")
    content = tmpl % {"game_path": GAME, "mcp_port": PORT, "logfile": logfile}
    tmpdir = tempfile.mkdtemp(prefix=f"run_{TARGET}_")
    ini = os.path.join(tmpdir, "scummvm.ini")
    open(ini, "w").write(content)
    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = "dummy"
    out = open(os.path.join("logs", f"scummvm_{TARGET}_{PORT}.out"), "w")
    return subprocess.Popen([BIN, "-c", ini, TARGET], env=env, stdout=out, stderr=subprocess.STDOUT)


def texts(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def state(c):
    try:
        return c.state()
    except Exception:
        return {}


def rid(c):
    return (state(c).get("room") or {}).get("id")


def skip_to_control(c):
    last = None
    for _ in range(30):
        try:
            c.skip()
        except Exception:
            pass
        time.sleep(1.0)
        s = state(c)
        room = (s.get("room") or {}).get("id")
        if s.get("verbs") and room == last and room is not None:
            return room
        last = room
    return last


def drain_dialog(c, tag):
    """Answer pending questions (pick the last choice) until none remain."""
    for _ in range(10):
        s = state(c)
        q = s.get("question")
        if not isinstance(q, dict):
            return
        choices = q.get("choices", [])
        print(f"   [{tag}] choices:", [ch.get("label") for ch in choices], flush=True)
        cid = choices[-1].get("id") if choices else 1
        r = c.answer(cid)
        for t in texts(r):
            print(f"      > {t}", flush=True)
        time.sleep(1.0)


def main():
    proc = launch()
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        room = skip_to_control(c)
        print("CONTROL room", room, "inv", state(c).get("inventory"), flush=True)

        # Trigger and play out the Largo toll-bridge encounter.
        r = c.act("walk_to", "path")
        for t in texts(r):
            print("  >", t, flush=True)
        time.sleep(1.5)
        drain_dialog(c, "largo")
        time.sleep(2.0)
        print("AFTER LARGO: room", rid(c), "inv", state(c).get("inventory"), flush=True)

        # Now explore Woodtick: walk to each exit, talk to the barkeep, log all.
        seen = {rid(c)}
        for step in range(14):
            s = state(c)
            if isinstance(s.get("question"), dict):
                drain_dialog(c, f"step{step}")
                continue
            room = (s.get("room") or {}).get("id")
            names = [o.get("name") for o in s.get("objects", [])]
            print(f"\n[step {step}] room={room} objs={names}", flush=True)
            # Prefer talking to a barkeep/person, else take an exit we haven't used.
            target = None
            for n in ("bartender", "barkeep", "man", "pirate"):
                if n in names:
                    r = c.act("talk_to", n)
                    print(f"  talk_to {n}:", texts(r), flush=True)
                    target = n
                    break
            if target is None:
                for n in ("path", "door", "ship", "stairs", "hatch", "entrance"):
                    if n in names:
                        before = room
                        r = c.act("walk_to", n)
                        time.sleep(1.8)
                        after = rid(c)
                        print(f"  walk_to {n} -> room {after} msgs {texts(r)}", flush=True)
                        if after != before and after not in seen:
                            seen.add(after)
                        break
            time.sleep(1.0)
        print("\nSEEN ROOMS:", sorted(x for x in seen if x is not None), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
