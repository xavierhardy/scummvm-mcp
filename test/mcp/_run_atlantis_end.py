"""Fate of Atlantis demo: reach the tire kit, then probe for the demo end."""

import sys
import time

from utils import MCP_HOST, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/Indy4Dem"
PORT = 23458
POLL = 0.5


def st(c):
    try:
        return c.state()
    except Exception:
        return {}


def rid(c):
    return (st(c).get("room") or {}).get("id")


def texts(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def main():
    proc = launch_scummvm("atlantis", GAME, port=PORT, scummvm_binary=BIN)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        # intro skip (as in _run_atlantis.py)
        result = c.skip()
        for _ in range(60):
            if "room_changed" in result:
                break
            time.sleep(POLL)
            result = c.skip()
        old = result.get("room_changed")
        for _ in range(60):
            if rid(c) != old:
                old = rid(c)
                break
            time.sleep(POLL)
        for _ in range(60):
            if rid(c) != old:
                break
            time.sleep(POLL)
        c.skip()
        c.answer(4)
        c.act("talk_to", "sophia")
        time.sleep(0.5)
        if isinstance(st(c).get("question"), dict):
            c.answer(4)
        c.act("walk_to", "path away from dock")
        time.sleep(0.5)
        c.answer(2)
        time.sleep(1.0)
        for name in ("notch in mountain", "cleft in mountain", "gap in mountain"):
            r = c.act("walk_to", name)
            if r.get("room_changed"):
                break
        kit = c.act("pick_up", "tire repair kit")
        print("tire kit:", kit.get("inventory_added"), "room", rid(c), flush=True)

        # --- probe past the tire kit for the demo end ---
        # Head back toward the dock, taking whatever exit each room exposes and
        # dumping the layout so the dock's "leave Thera" hotspot is discoverable.
        EXITS = (
            "path_back_to_the_dock",
            "path_to_dock",
            "path_to_landscape",
            "path away from dock",
            "path to dock",
            "dock",
            "path",
            "down",
            "exit",
            "boat",
            "balloon",
            "raft",
            "ship",
        )
        seen = []
        for step in range(12):
            s = st(c)
            room = (s.get("room") or {}).get("id")
            names = [o.get("name") for o in s.get("objects", [])]
            seen.append(room)
            print(f"[{step}] room={room} objs={names}", flush=True)
            # At the dock, the salvage boat / captain is how Indy leaves Thera.
            if room == 49:
                for n in ("captain", "salvage_boat", "boat", "sophia"):
                    if n in names:
                        for verb in ("talk_to", "use"):
                            r = c.act(verb, n)
                            tt = texts(r)
                            if tt:
                                print(f"   {verb} {n}: {tt}", flush=True)
                            time.sleep(1.0)
                            for _ in range(8):
                                q = st(c).get("question")
                                if not isinstance(q, dict):
                                    break
                                labels = [x.get("label") for x in q.get("choices", [])]
                                print("    Q:", labels, flush=True)
                                rr = c.answer(q["choices"][-1]["id"])
                                if texts(rr):
                                    print("      >", texts(rr), flush=True)
                                time.sleep(1.0)
                            if rid(c) != 49:
                                print(f"   -> LEFT THERA, room {rid(c)}", flush=True)
                                break
                        if rid(c) != 49:
                            break
                print("DOCK FINAL room:", rid(c), flush=True)
                break
            moved = False
            for n in EXITS:
                if n in names:
                    r = c.act("walk_to", n)
                    time.sleep(1.8)
                    if texts(r):
                        print(f"   walk {n}: {texts(r)}", flush=True)
                    if rid(c) != room:
                        moved = True
                        break
            if not moved:
                print("   (no further exit found)", flush=True)
                break
            time.sleep(0.6)
        print("ROOMS BACK:", seen, "FINAL room:", rid(c), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
