"""Drive the Full Throttle DOS demo from the dumpster to the end (get the keys)."""

import sys
import time

from utils import MCP_HOST, McpClient, get_state_with_retry, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/ft-dos-demo"
PORT = 23461

ALLEY, BAR_FRONT, BAR = 10, 6, 7


def room(c: McpClient) -> int:
    return (get_state_with_retry(c).get("room") or {}).get("id")


def act_retry(c: McpClient, verb: str, target, attempts: int = 15) -> dict:
    last = None
    for _ in range(attempts):
        try:
            return c.act(verb, target)
        except RuntimeError as e:
            last = e
            if "not accepting input" in str(e):
                time.sleep(1.0)
                continue
            raise
    print(f"  !! act({verb},{target}) never accepted input: {last}")
    return {}


def walk_click(c: McpClient, x: int, y: int) -> None:
    try:
        c.walk(x, y)
    except RuntimeError as e:
        if "not accepting input" not in str(e):
            raise


def texts(result: dict) -> list[str]:
    return [m.get("text", "") for m in (result or {}).get("messages", [])]


def main() -> None:
    proc = launch_scummvm("ft-demo", GAME, port=PORT, scummvm_binary=BIN)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)

        # 1. Skip the intro until the dumpster alley (room 10).
        for _ in range(25):
            if room(c) == ALLEY:
                break
            time.sleep(0.6)
            try:
                c.skip()
            except Exception:
                pass
        print(f"[intro] room={room(c)} (want {ALLEY})", flush=True)

        # 2. Climb out of the dumpster and leave the alley -> bar front (6).
        time.sleep(2)
        for _ in range(12):
            for pt in ((160, 50), (240, 50), (300, 75)):
                walk_click(c, *pt)
                time.sleep(0.5)
            if room(c) == BAR_FRONT:
                break
        print(f"[alley] room={room(c)} (want {BAR_FRONT})", flush=True)

        # 3. Punch then kick the locked door open.
        print("  punch door:", texts(act_retry(c, "fist", "door")), flush=True)
        kick = act_retry(c, "kick", "door")
        print("  kick door:", kick.get("objects_changed"), flush=True)

        # 4. Walk through the doorway into the bar (7).
        for _ in range(10):
            walk_click(c, 204, 80)
            time.sleep(1.5)
            if room(c) == BAR:
                break
        print(f"[bar] room={room(c)} (want {BAR})", flush=True)

        # 5. Look at the antlers, then punch the bartender for the keys.
        print("  antlers:", texts(act_retry(c, "mouth", "antlers")), flush=True)
        res = act_retry(c, "fist", "bartender")
        blob = " ".join(texts(res))
        for _ in range(40):
            if "your keys" in blob:
                break
            time.sleep(2)
            blob += " " + " ".join(
                m.get("text", "") for m in get_state_with_retry(c).get("messages", [])
            )
        got = "your keys" in blob
        print(f"RESULT: got_keys={got} | room={room(c)}", flush=True)
        # tail of the interrogation transcript
        print("  transcript tail:", blob[-300:], flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
