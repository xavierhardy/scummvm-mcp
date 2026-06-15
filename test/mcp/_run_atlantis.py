"""Drive the Fate of Atlantis demo: Thera dock -> canyon -> cleft -> tire kit."""

import sys
import time

from utils import MCP_HOST, McpClient, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/Indy4Dem"
PORT = 23458
POLL = 0.5


def room(c: McpClient):
    return (c.state().get("room") or {}).get("id")


def texts(result: dict) -> list[str]:
    return [m.get("text", "") for m in (result or {}).get("messages", [])]


def question(c: McpClient):
    q = c.state().get("question")
    return q if isinstance(q, dict) else None


def main() -> None:
    proc = launch_scummvm("atlantis", GAME, port=PORT, scummvm_binary=BIN)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)

        # 1. Skip the intro: skip until a room change, then wait through the two
        #    intro screens (matches test_atlantis.py).
        result = c.skip()
        for _ in range(60):
            if "room_changed" in result:
                break
            time.sleep(POLL)
            result = c.skip()
        old = result.get("room_changed")
        for _ in range(60):
            new = room(c)
            if new != old:
                old = new
                break
            time.sleep(POLL)
        for _ in range(60):
            new = room(c)
            if new != old:
                break
            time.sleep(POLL)
        c.skip()
        print(f"[dock] room={room(c)} question={bool(question(c))}", flush=True)

        # 2. Answer the opening dialog (take a look around).
        print("  open:", texts(c.answer(4)), flush=True)

        # 3. Talk to Sophia, then close her dialog.
        c.act("talk_to", "sophia")
        time.sleep(0.5)
        if question(c):
            c.answer(4)

        # 4. Walk up the path; Sophia waits, Indy heads off to look for Kerner.
        c.act("walk_to", "path away from dock")
        time.sleep(0.5)
        kerner = c.answer(2)
        print("  kerner:", texts(kerner), "room->", kerner.get("room_changed"), flush=True)

        # 5. Walk to the cleft in the mountain (several aliases).
        moved = None
        for name in ("notch in mountain", "cleft in mountain", "gap in mountain"):
            r = c.act("walk_to", name)
            if r.get("room_changed"):
                moved = r.get("room_changed")
                break
        print(f"[mountain] room={room(c)} (moved->{moved})", flush=True)

        # 6. Pick up the tire repair kit by the wrecked truck.
        kit = c.act("pick_up", "tire repair kit")
        print("RESULT: tire kit ->", kit.get("inventory_added"), "| room", room(c), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
