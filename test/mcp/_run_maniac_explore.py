"""Explore the Maniac Mansion C64 demo past the front door to find the demo end."""

import sys
import time

from utils import MCP_HOST, McpClient, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/ManiacMansionDemo/Games/ManiacMansion"
PORT = 23457


def objs(s):
    out = []
    for o in s.get("objects", []):
        out.append(
            (o.get("name"), o.get("position") or (o.get("x"), o.get("y")),
             o.get("pathway"), o.get("compatible_verbs"))
        )
    return out


def texts(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


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
        s = c.state()
        print("ENTERED room", (s.get("room") or {}).get("id"), flush=True)
        print("controlling:", s.get("controlling"), "chars:", s.get("available_characters"), flush=True)
        for row in objs(s):
            print("  obj:", row, flush=True)

        # Try going up the stairs (should be blocked per "can't go upstairs").
        up = c.act("walk_to", "stairs")
        print("\nSTAIRS ->", up.get("room_changed"), "msgs:", texts(up), flush=True)
        time.sleep(1)
        print("  room after stairs:", (c.state().get("room") or {}).get("id"), flush=True)

        # Explore the ground floor: walk to each interior door by coordinate.
        visited = {(s.get("room") or {}).get("id")}
        s = c.state()
        doors = [o for o in s.get("objects", []) if o.get("name") in ("door",) or o.get("pathway")]
        for d in doors:
            pos = d.get("position") or {}
            x, y = pos.get("x"), pos.get("y")
            if x is None:
                continue
            print(f"\n-> walk to door at ({x},{y})", flush=True)
            try:
                r = c.walk(x, y)
            except Exception as e:
                print("   walk err:", e); continue
            time.sleep(1.5)
            ns = c.state()
            rid = (ns.get("room") or {}).get("id")
            print(f"   room={rid} msgs={texts(r)}", flush=True)
            if rid not in visited:
                visited.add(rid)
                print("   NEW ROOM objects:", [o.get("name") for o in ns.get("objects", [])], flush=True)
                # go back to the hall
                back = [o for o in ns.get("objects", []) if o.get("pathway") or o.get("name") == "door"]
                if back:
                    bp = back[0].get("position") or {}
                    if bp.get("x") is not None:
                        c.walk(bp["x"], bp["y"]); time.sleep(1.5)
        print("\nVISITED ROOMS:", sorted(r for r in visited if r is not None), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
