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

        # Talk to the troll first so Guybrush learns of the "magic words"; this
        # unlocks the prisoner's "magic phrase" topic in the prison checkpoint.
        # Answering once closes the dialog so the following walks aren't blocked.
        client.walk(120, 132)
        client.act("talk_to", "Troll")
        client.answer(3)
        time.sleep(1.0)

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


def capture_pass(port: int = 23992) -> None:
    """Drive the Indiana Jones (Last Crusade) demo and park a save at the very
    last step, so the Venice travel goal can be tested by hand.

    Loads the Indy3 start (slot 3) and replays the full solve from
    ``IndyRealHarness`` (gym -> corridor -> outside -> Henry's ransack ->
    office mob -> Indy's office mail/grail-diary + sticky-tape->small-key ->
    back to Henry's chest for the old book + the painting), then stops at the
    outside travel hub (room 24) holding ``grail_diary`` + ``old_book`` +
    ``small_key`` + ``painting`` and saves to slot 5. The painting is the
    Grail-quest trigger, so from this save the ``travel`` verb already offers
    "To the Plane to Venice" -- load ``pass.s05`` and travel there to end the
    demo.
    """
    proc = launch_scummvm(
        "pass",
        GAME_PATHS["pass"],
        port=port,
        scummvm_binary=BINARY,
        save_slot=3,             # the Indy3 interactive start
        isolate_saves=False,     # write pass.s05 into the repo save_slots dir
    )

    def act(verb, t1=None, t2=None, tries=15):
        # Retries cover transient "not accepting input" while scripts / the
        # mob-banging cutscene hold input.
        data: dict = {}
        for _ in range(tries):
            try:
                data = client.act(verb, t1, t2)
            except RuntimeError:
                time.sleep(0.8)
                continue
            if isinstance(data, dict) and data.get("error"):
                time.sleep(0.8)
                continue
            return data if isinstance(data, dict) else {}
        return data if isinstance(data, dict) else {}

    def go(target, dest, opens=False, tries=14):
        if opens:
            act("open", target)
        for _ in range(tries):
            r = act("walk to", target)
            if (isinstance(r, dict) and r.get("room_changed") == dest) or _room(client) == dest:
                return True
            time.sleep(0.5)
        return _room(client) == dest

    def travel_to(keyword, dest, tries=8):
        for _ in range(tries):
            r = act("travel")
            q = (r.get("question") if isinstance(r, dict) else None) or client.state().get("question")
            if isinstance(q, dict):
                cid = next((c["id"] for c in q["choices"] if keyword in c["label"].lower()), None)
                if cid is None:
                    return False  # destination not offered
                client.answer(cid)
            if _wait_room(client, dest, tries=8):
                return True
        return _room(client) == dest

    try:
        client = wait_for_mcp(MCP_HOST, port, timeout=30)
        assert _wait_room(client, 25), f"expected the gym (room 25), got {_room(client)}"

        # Faster route: straight to the office via door 103 (never open the left
        # door / go outside first -- the outside is reached via the window).
        go(213, 20)                  # gym -> corridor
        act("open", 103)
        go(103, 22)                  # corridor door 103 -> office
        # Calm the student mob (the "take down names" line opens Indy's office).
        prefer = ("work something out", "calmly", "take it easy", "fair for everyone")
        resolve = "take down names"
        act("talk to", "students")
        for _ in range(8):
            q = client.state().get("question")
            if not isinstance(q, dict):
                break
            cid = next(
                (c["id"] for c in q["choices"] if any(k in c["label"].lower() for k in prefer)),
                None,
            ) or next((c["id"] for c in q["choices"] if resolve in c["label"].lower()), None)
            if cid is None:
                break
            client.answer(cid)
            time.sleep(0.6)
        assert _wait_room(client, 21, tries=8), f"expected Indy's office (21), got {_room(client)}"

        # Mail chain -> grail diary, then open the window (only once we have the
        # diary) and climb out -- first time out plays the Donovan cutscene.
        for item in ("junk_mail", "letters", "papers", "package"):
            act("pick_up", item)
        act("open", "package")                     # grail_diary
        act("open", "window")
        go("window", 24)                           # climb out to the outside
        saw = False
        for _ in range(80):
            s = client.state()
            rid = (s.get("room") or {}).get("id")
            if rid == 29:
                saw = True
            elif rid == 24 and "travel" in (s.get("verbs") or []):
                break
            time.sleep(1.0)

        # Henry's #1: painting + sticky tape (leave the plant AND the cloth --
        # the cloth can't be pulled until the plant is moved, on the last trip).
        assert travel_to("henry", 27), "could not travel to Henry's house"
        act("pick_up", "painting")
        act("pull", "bookcase")
        act("pick_up", "sticky_tape")
        # Back to the office through the window: sticky tape on the jar -> key.
        go(231, 24)                                # Henry's -> outside
        go("window", 21)                           # outside -> office through the window
        act("use", "sticky_tape", "jar")           # small_key (added async)
        go("window", 24)                           # back outside
        # Henry's #2 (last trip), in order: move the plant, pull the cloth (only
        # possible after the plant) to reveal the chest, then open it.
        assert travel_to("henry", 27), "could not travel back to Henry's house"
        act("pick_up", "plant")                    # only after the key
        act("pull", "table_cloth")                 # now reveals the chest
        act("use", "small_key", "chest")
        act("pick_up", "old_book")                 # last Grail item -> unlocks Venice
        go(231, 24)                                # out to the travel hub

        assert _room(client) == 24, f"expected the travel hub (room 24), got {_room(client)}"
        inv = client.state().get("inventory")
        print(f"  inventory before save: {inv}")
        _save(client, 5, "indy3 pre-venice: outside (24), grail_diary+old_book+small_key+painting")

        # Probe the travel menu so we can see what destinations it offers here.
        r = act("travel")
        q = (r.get("question") if isinstance(r, dict) else None) or client.state().get("question")
        if isinstance(q, dict):
            print(f"  travel menu offers: {[c['label'] for c in q['choices']]}")
            cancel = next((c["id"] for c in q["choices"] if "cancel" in c["label"].lower()), None)
            if cancel is not None:
                client.answer(cancel)

        client.close()
        print("pass (Indy3) checkpoint captured: slot 5 (load pass.s05 and use 'travel')")
    finally:
        proc.kill()


CAPTURERS = {"monkey": capture_monkey, "pass": capture_pass}


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
