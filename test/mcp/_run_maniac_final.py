"""Maniac C64 demo: enter, map the ground floor, confirm the demo-end wall."""

import sys
import time

from utils import MCP_HOST, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/ManiacMansionDemo/Games/ManiacMansion"
PORT = 23457


def txt(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def rid(c):
    return (c.state().get("room") or {}).get("id")


def main():
    proc = launch_scummvm("maniac-c64", GAME, port=PORT, scummvm_binary=BIN, save_slot=1)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        c.act("walk_to", "front_door")
        c.act("pull", "door mat")
        c.act("pick_up", "key")
        c.act("use", "key", "front_door")
        c.act("walk_to", "front_door")
        time.sleep(2)
        print("entered room", rid(c), flush=True)

        # Map the three hall doors: position by each, open, step through, dump, return.
        hall_objs = [o.get("name") for o in c.state().get("objects", [])]
        print("hall objects:", hall_objs, flush=True)
        for (x, y) in ((20, 52), (56, 52), (73, 60)):
            if rid(c) != 10:
                # return to the hall first
                for n in c.state().get("objects", []):
                    if n.get("name") in ("door", "stairs"):
                        c.act("open", "door")
                        c.act("walk_to", "door")
                        time.sleep(1.5)
                        break
            c.walk(x, y)
            time.sleep(1.2)
            c.act("open", "door")
            r = c.act("walk_to", "door")
            time.sleep(1.5)
            room = rid(c)
            print(f"door@({x},{y}) -> room {room} msgs {txt(r)}", flush=True)
            if room != 10:
                objs = [o.get("name") for o in c.state().get("objects", [])]
                print(f"   room {room} objects: {objs}", flush=True)
                # return to hall
                c.act("open", "door")
                c.act("walk_to", "door")
                time.sleep(1.5)
                print("   back in room", rid(c), flush=True)

        # Confirm the demo wall at the staircase.
        up = c.act("walk_to", "stairs")
        print("\nSTAIRS:", txt(up), "| room", rid(c), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
