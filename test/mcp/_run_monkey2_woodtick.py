"""MI2 demo: play through Largo, then map Woodtick to find the bar/barkeep + end."""

import os
import subprocess
import sys
import tempfile
import time

from utils import MCP_HOST, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/monkey2-dos-ni-demo-en"
PORT = 23483
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


def safe(fn, *a, **k):
    try:
        return fn(*a, **k)
    except Exception as e:
        return {"_err": str(e)}


def st(c):
    s = safe(c.state)
    return s if isinstance(s, dict) else {}


def rid(c):
    return (st(c).get("room") or {}).get("id")


def texts(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def answer(c, cid):
    for _ in range(4):
        r = safe(c.answer, cid)
        if "_err" not in r:
            return r
        safe(c.skip)
        time.sleep(1.2)
    return {}


def drain(c, tag, picks=("last",)):
    out = []
    for _ in range(12):
        q = st(c).get("question")
        if not isinstance(q, dict):
            break
        ch = q.get("choices", [])
        cid = ch[-1]["id"] if ch else 1
        r = answer(c, cid)
        out += texts(r)
    if out:
        print(f"   [{tag}]:", " | ".join(out), flush=True)
    return out


def main():
    proc = launch()
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        # skip rolling intro
        last = None
        for _ in range(30):
            safe(c.skip)
            time.sleep(1.0)
            s = st(c)
            r = (s.get("room") or {}).get("id")
            if s.get("verbs") and r == last and r is not None:
                break
            last = r
        print("CONTROL room", rid(c), "inv", st(c).get("inventory"), flush=True)

        # Largo bridge encounter
        safe(c.act, "walk_to", "path")
        time.sleep(1.5)
        drain(c, "largo")
        time.sleep(2.5)
        print("AFTER LARGO room", rid(c), "inv", st(c).get("inventory"), flush=True)

        # Explore Woodtick. Track rooms; talk to NPCs; watch for end signals.
        seen = {}
        for step in range(22):
            if isinstance(st(c).get("question"), dict):
                drain(c, f"s{step}")
                continue
            s = st(c)
            r = (s.get("room") or {}).get("id")
            names = [o.get("name") for o in s.get("objects", [])]
            seen.setdefault(r, names)
            print(f"[{step}] room={r} objs={names}", flush=True)
            # talk to any NPC
            spoke = False
            for n in ("bartender", "barkeep", "barkeeper", "man", "pirate", "wally"):
                if n in names:
                    rr = safe(c.act, "talk_to", n)
                    print(f"   talk {n}:", texts(rr), flush=True)
                    drain(c, f"talk{step}")
                    spoke = True
                    break
            if spoke:
                continue
            # else take an exit
            moved = False
            for n in ("path", "ship", "door", "hatch", "entrance", "stairs"):
                cnt = names.count(n)
                for _i in range(cnt):
                    before = r
                    rr = safe(c.act, "walk_to", n)
                    if "_err" in rr:
                        continue
                    time.sleep(1.8)
                    after = rid(c)
                    if texts(rr):
                        print(f"   walk {n}: {texts(rr)}", flush=True)
                    if after != before:
                        moved = True
                        break
                if moved:
                    break
            time.sleep(0.8)
        print("\nROOMS SEEN:", sorted(k for k in seen if k is not None), flush=True)
        for k in sorted(seen):
            print(f"  room {k}: {seen[k]}", flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
