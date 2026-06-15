"""Drive the Maniac Mansion C64 demo to the end (get into the mansion)."""

import sys
import time

from utils import MCP_HOST, McpClient, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/ManiacMansionDemo/Games/ManiacMansion"
PORT = 23457


def show(c: McpClient, label: str) -> dict:
    s = c.state()
    room = s.get("room")
    print(f"[{label}] room={room} inv={s.get('inventory')}", flush=True)
    return s


def main() -> None:
    proc = launch_scummvm("maniac-c64", GAME, port=PORT, scummvm_binary=BIN, save_slot=1)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        show(c, "start")
        print("walk:", c.act("walk_to", "front_door").get("position"), flush=True)
        print("pull mat:", c.act("pull", "door mat").get("objects_changed"), flush=True)
        print("pick key:", c.act("pick_up", "key").get("inventory_added"), flush=True)
        print("use key:", c.act("use", "key", "front_door").get("objects_changed"), flush=True)
        r = c.act("walk_to", "front_door")
        print("enter ->", r.get("room_changed"), flush=True)
        time.sleep(2)
        s = show(c, "inside")
        # Confirm the demo boundary: list objects/exits available on the ground floor.
        names = [o.get("name") for o in s.get("objects", [])]
        print("objects inside:", names, flush=True)
        room_id = (s.get("room") or {}).get("id")
        print(f"RESULT: reached room {room_id} (mansion entrance)", flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
