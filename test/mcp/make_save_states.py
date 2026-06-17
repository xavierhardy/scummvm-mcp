#!/usr/bin/env python3
"""Capture checkpoint save states used by the parallel MCP test suite.

Some games are deep sequential walkthroughs where later tests need rooms/items
established by earlier steps. Under ``--dist=loadgroup`` each test runs on its
own fresh instance, so instead of replaying the whole prefix every time we load
a checkpoint save created here once. The checkpoints are written straight into
``test/mcp/save_slots/<game>/<game>.sNN`` (launch with ``isolate_saves=False``)
via the ``save_state`` MCP tool (requires ``mcp_debug=true``, which the test ini
files set).

Run this on a machine that HAS the game data (e.g. the Pi), then commit the
generated ``.sNN`` files:

    python make_save_states.py monkey      # capture the Monkey 1 checkpoints

Currently only ``monkey`` (English MI1 EGA demo) is implemented; add more games
by following the same pattern.
"""
import os
import sys
import time

from utils import GAME_PATHS, MCP_HOST, launch_scummvm, wait_for_mcp
from mcp_cli import _post_tool

BINARY = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")


def _room(client) -> object:
    r = client.state().get("room") or {}
    return r.get("id")


def _wait_room(client, target: int, tries: int = 25) -> bool:
    for _ in range(tries):
        if _room(client) == target:
            return True
        time.sleep(0.6)
    return False


def _save(client, slot: int, desc: str) -> None:
    res = _post_tool(client, "save_state", {"slot": slot, "description": desc})
    if not res.get("saved"):
        raise SystemExit(f"save_state(slot={slot}) refused: {res}")
    time.sleep(1.0)  # let the deferred save flush before the next action
    print(f"  saved slot {slot} ({desc}) in room {_room(client)}")


def capture_monkey(port: int = 23991) -> None:
    """Drive the English MI1 EGA demo and capture the bar/kitchen/prison saves.

    Mirrors the documented walkthrough in test_monkey.py:
      55 -> door -> 52 (bar)        -> slot 6
      pick up bowl, 354 -> 51 (kitchen) -> slot 7  (breath mint in inventory)
      navigate to the prison (room 54)  -> slot 8  (mint in inventory)
    """
    proc = launch_scummvm(
        "monkey-ega-demo",
        GAME_PATHS["monkey-ega-demo"],
        port=port,
        scummvm_binary=BINARY,
        save_slot=1,
        isolate_saves=False,  # write checkpoints into the repo save_slots dir
    )
    try:
        client = wait_for_mcp(MCP_HOST, port, timeout=30)
        assert _wait_room(client, 55), f"expected room 55, got {_room(client)}"

        # 55 -> SCUMM bar (room 52).
        client.act("walk_to", "door")
        client.act("open", "door")
        client.act("walk_to", "door")
        assert _wait_room(client, 52), f"expected room 52, got {_room(client)}"
        _save(client, 6, "monkey bar (room 52)")

        # Grab the bowl (breath mint), then kitchen (room 51).
        client.act("pick_up", "bowl o' mints")
        client.act("open", 354)
        client.act("walk_to", 354)
        assert _wait_room(client, 51), f"expected room 51, got {_room(client)}"
        _save(client, 7, "monkey kitchen (room 51), mint in inventory")

        # Navigate to the prison (room 54).
        client.act("walk", 305)            # -> 52
        _wait_room(client, 52)
        client.act("walk", 353)            # -> 55
        _wait_room(client, 55)
        client.act("walk", "archway")      # -> 57
        _wait_room(client, 57)
        for _ in range(6):
            if _room(client) == 54:
                break
            try:
                client.act("walk", "jail_entrance")  # -> 54
            except RuntimeError:
                pass
            time.sleep(0.8)
        assert _room(client) == 54, f"expected the prison (room 54), got {_room(client)}"
        _save(client, 8, "monkey prison (room 54), mint in inventory")

        client.close()
        print("monkey checkpoints captured: slots 6, 7, 8")
    finally:
        proc.kill()


CAPTURERS = {"monkey": capture_monkey}


def main() -> None:
    games = sys.argv[1:] or ["monkey"]
    for game in games:
        fn = CAPTURERS.get(game)
        if fn is None:
            raise SystemExit(f"no capturer for {game!r}; known: {sorted(CAPTURERS)}")
        print(f"capturing {game} checkpoints...")
        fn()


if __name__ == "__main__":
    main()
