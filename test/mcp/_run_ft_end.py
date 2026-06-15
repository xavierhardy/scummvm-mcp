"""Full Throttle demo: reach the keys, then probe past them for the demo end."""

import sys
import time

from utils import MCP_HOST, McpClient, get_state_with_retry, launch_scummvm, wait_for_mcp

BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/ft-dos-demo"
PORT = 23461
ALLEY, BAR_FRONT, BAR = 10, 6, 7


def room(c):
    return (get_state_with_retry(c).get("room") or {}).get("id")


def texts(r):
    return [m.get("text", "") for m in (r or {}).get("messages", [])]


def act_retry(c, verb, target, attempts=15):
    for _ in range(attempts):
        try:
            return c.act(verb, target)
        except RuntimeError as e:
            if "not accepting input" in str(e):
                time.sleep(1.0)
                continue
            raise
    return {}


def walk_click(c, x, y):
    try:
        c.walk(x, y)
    except RuntimeError as e:
        if "not accepting input" not in str(e):
            raise


def main():
    proc = launch_scummvm("ft-demo", GAME, port=PORT, scummvm_binary=BIN)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        for _ in range(25):
            if room(c) == ALLEY:
                break
            time.sleep(0.6)
            try:
                c.skip()
            except Exception:
                pass
        time.sleep(2)
        for _ in range(12):
            for pt in ((160, 50), (240, 50), (300, 75)):
                walk_click(c, *pt)
                time.sleep(0.5)
            if room(c) == BAR_FRONT:
                break
        act_retry(c, "fist", "door")
        act_retry(c, "kick", "door")
        for _ in range(10):
            walk_click(c, 204, 80)
            time.sleep(1.5)
            if room(c) == BAR:
                break
        act_retry(c, "mouth", "antlers")
        res = act_retry(c, "fist", "bartender")
        blob = " ".join(texts(res))
        for _ in range(40):
            if "your keys" in blob:
                break
            time.sleep(2)
            blob += " " + " ".join(
                m.get("text", "") for m in get_state_with_retry(c).get("messages", [])
            )
        print(f"got_keys={'your keys' in blob} room={room(c)}", flush=True)

        # --- probe past the keys for the demo end ---
        # let the close-up cutscene return to the bar
        for _ in range(20):
            if room(c) == BAR:
                break
            time.sleep(2)
        print("settled room:", room(c), flush=True)
        s = get_state_with_retry(c)
        print("bar objects:", [o.get("name") for o in s.get("objects", [])], flush=True)

        # Leave the bar -> bar front, then try to ride the bike.
        for _ in range(8):
            walk_click(c, 30, 110)  # toward the bar exit (left)
            time.sleep(1.2)
            if room(c) == BAR_FRONT:
                break
        print("after-exit room:", room(c), flush=True)
        s = get_state_with_retry(c)
        front_objs = [o.get("name") for o in s.get("objects", [])]
        print("front objects:", front_objs, flush=True)
        for verb in ("interact", "use item", "walk to"):
            if room(c) != BAR_FRONT:
                break
            r = act_retry(c, verb, "bike")
            print(f"  {verb} bike -> room {room(c)} msgs {texts(r)}", flush=True)
            time.sleep(2)
        # Observe for a terminal cutscene / message for a while.
        for _ in range(20):
            s = get_state_with_retry(c)
            msgs = [m.get("text", "") for m in s.get("messages", [])]
            if msgs:
                print("   end-msgs:", msgs, "room", (s.get("room") or {}).get("id"), flush=True)
            time.sleep(2)
        print("FINAL room:", room(c), flush=True)
    finally:
        proc.kill()
        proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
