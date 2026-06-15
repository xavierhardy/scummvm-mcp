"""Map the Maniac C64 demo ground floor and confirm the demo-end wall."""

import sys
import time

from utils import MCP_HOST, McpClient, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/ManiacMansionDemo/Games/ManiacMansion"
PORT = 23457


def texts(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def rid(c):
    return (c.state().get("room") or {}).get("id")


def enter(c):
    c.act("walk_to", "front_door")
    c.act("pull", "door mat")
    c.act("pick_up", "key")
    c.act("use", "key", "front_door")
    c.act("walk_to", "front_door")
    time.sleep(2)


def main():
    proc = launch_scummvm("maniac-c64", GAME, port=PORT, scummvm_binary=BIN, save_slot=1)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        enter(c)
        print("entered room", rid(c), flush=True)

        # Walk Dave to each interior door, open it, and step through.
        for (x, y) in ((20, 52), (56, 52), (73, 60)):
            c.walk(x, y)
            time.sleep(1.2)
            opened = c.act("open", "door")
            time.sleep(0.6)
            through = c.walk(x, y)
            time.sleep(1.5)
            r = rid(c)
            print(f"door({x},{y}) open={texts(opened)} -> room {r}", flush=True)
            if r != 10:
                s = c.state()
                print("   GROUND-FLOOR ROOM objects:",
                      [o.get("name") for o in s.get("objects", [])], flush=True)
                # walk back through the same door to the hall
                back = next((o for o in s.get("objects", [])
                             if o.get("name") in ("door",) or o.get("pathway")), None)
                if back:
                    p = back.get("position") or {}
                    if p.get("x") is not None:
                        c.act("open", "door")
                        c.walk(p["x"], p["y"])
                        time.sleep(1.5)
                print("   back in room", rid(c), flush=True)

        # The demo end: the staircase is walled off until you buy the game.
        up = c.act("walk_to", "stairs")
        print("\nDEMO END at stairs:", texts(up), "| room", rid(c), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
