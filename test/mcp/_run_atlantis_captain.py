"""Atlantis demo: reach the dock and probe the captain (leave-Thera mechanism)."""

import sys
import time

from utils import MCP_HOST, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/Indy4Dem"
PORT = 23458
POLL = 0.5


def safe(fn, *a):
    for _ in range(4):
        try:
            return fn(*a)
        except Exception as e:
            if "stream ended" in str(e) or "timed out" in str(e) or "not accepting" in str(e):
                time.sleep(1.2)
                continue
            return {"_err": str(e)}
    return {"_err": "retries exhausted"}


def stt(c):
    s = safe(c.state)
    return s if isinstance(s, dict) else {}


def rid(c):
    return (stt(c).get("room") or {}).get("id")


def txt(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def main():
    proc = launch_scummvm("atlantis", GAME, port=PORT, scummvm_binary=BIN)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
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
        safe(c.answer, 4)
        safe(c.act, "talk_to", "sophia")
        time.sleep(0.5)
        if isinstance(stt(c).get("question"), dict):
            safe(c.answer, 4)
        safe(c.act, "walk_to", "path away from dock")
        time.sleep(0.5)
        safe(c.answer, 2)
        time.sleep(1.0)
        for name in ("notch in mountain", "cleft in mountain", "gap in mountain"):
            r = safe(c.act, "walk_to", name)
            if isinstance(r, dict) and r.get("room_changed"):
                break
        safe(c.act, "pick_up", "tire repair kit")
        # back to the dock
        safe(c.act, "walk_to", "path_to_landscape")
        time.sleep(1.5)
        safe(c.act, "walk_to", "path_back_to_the_dock")
        time.sleep(1.5)
        print("at dock? room", rid(c), "objs",
              [o.get("name") for o in stt(c).get("objects", [])], flush=True)

        # Talk to the captain and walk through the leave-Thera dialog.
        r = safe(c.act, "talk_to", "captain")
        print("talk captain:", txt(r), flush=True)
        for _ in range(14):
            q = stt(c).get("question")
            if not isinstance(q, dict):
                break
            labels = [(x.get("id"), x.get("label")) for x in q.get("choices", [])]
            print("  Q:", labels, flush=True)
            cid = q["choices"][-1]["id"]
            rr = safe(c.answer, cid)
            if txt(rr):
                print("    >", txt(rr), flush=True)
            time.sleep(1.2)
            if rid(c) != 49:
                print("  LEFT dock -> room", rid(c), flush=True)
                break
        # Observe for a terminal/demo-over message.
        for _ in range(10):
            s = stt(c)
            msgs = [m.get("text", "") for m in s.get("messages", [])]
            if msgs:
                print("  msg:", msgs, "room", (s.get("room") or {}).get("id"), flush=True)
            time.sleep(1.5)
        print("FINAL room", rid(c), "inv", stt(c).get("inventory"), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
