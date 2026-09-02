#!/usr/bin/env python3
"""Capture the checkpoint saves the game tests start from.

Most tests here begin just past a game's opening rather than at its title
screen, which is what ``save_slots/<game_id>/`` holds. Those files are captured
on a machine that has the game data and then committed, so a checkout without
the data skips rather than fails (see ``require_save_slot``).

This is the script that captures them, and it is the answer to "how was this
save made" for every file in that directory.

    ./make_save_states.py kq2 kyra1 sanitarium      # these games
    ./make_save_states.py --all                     # every configured game

Two things make it harder than "skip a few times and save".

A game refuses to save while its opening is still running, and says so in
those words - so the save attempt is itself the readiness test, retried on a
long budget because an opening can be many minutes of film. Escape is pressed
throughout: some openings take notice of it and some do not, and a good few
take notice of nothing at all.

Some games refuse however long it waits, and for three different reasons worth
knowing before spending seventeen minutes finding out:

  * the game's own opening simply never permits it - Loom, both Discworlds,
    Gobliins 2 and 3 and Ween were each asked for seventeen minutes and
    refused the whole way;
  * AGI only permits a save while the typing prompt is showing, and King's
    Quest III keeps it hidden - so it refuses even standing in a room taking
    commands;
  * SCI asks ``canSaveFromGMM()`` before anything else, which is false for
    most of its games: ScummVM will not save them from outside their own menu
    whatever state they are in;
  * and AGOS implements no ``canSaveGameStateCurrently()`` at all, so the base
    engine's answer - no - stands for every game it runs. Simon the Sorcerer
    can only be saved from its own menu, and does not need to be: its opening
    is a cutscene, and ``skip`` now ends it (see AGOSEngine::mcpExitCutscene).

Those games have no save here and start from scratch instead, which the
launcher works out for itself.

And ``can_act`` is not the same question. It is true through a good deal of a
cutscene, so it cannot say whether the player has control. Walking to where the
player already stands can: it is a real command through the real tool, it asks
the game to move nobody anywhere, and a game that takes it is a game taking
commands. That is the check reported beside each save.
"""

import argparse
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from launcher import (  # noqa: E402
    GAME_PATHS,
    launch_scummvm,
    save_slot_path,
)
from mcp_client import wait_for_mcp  # noqa: E402

#: Long enough for a CD game to read its resources before its first game cycle.
CONNECT_TIMEOUT_SECS = 600.0
#: How many times to ask, and how long to wait between asking.
DEFAULT_TRIES = 100
DEFAULT_SETTLE_SECS = 10.0
#: Escape only helps at the start, where the opening is still running; past
#: that, pressing it every few seconds is a player jabbing at a game that is
#: already waiting for them.
SKIP_TRIES = 5


def _in_control(client, state) -> bool:
    """Ask the game to walk the player to where the player already is."""
    position = state.get("position") or {}
    x, y = position.get("x"), position.get("y")
    if x is None or y is None:
        return False
    try:
        client.call_tool("walk", {"x": int(x), "y": int(y)})
    except RuntimeError:
        return False
    return True


def capture(game_id: str, slot: int, port: int, tries: int, settle: float) -> bool:
    """Take one game past its opening and write its save. True when it took."""
    path = GAME_PATHS.get(game_id)
    if not path:
        print(f"{game_id}: no game path configured", file=sys.stderr)
        return False

    scummvm = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scummvm")
    # isolate_saves=False so save_state writes into the repository's own
    # save_slots/<game_id>, which is the point of running this.
    proc = launch_scummvm(
        game_id, path, port=port, scummvm_binary=scummvm, save_slot=slot,
        isolate_saves=False,
    )
    try:
        client = wait_for_mcp("127.0.0.1", port, connect_timeout=CONNECT_TIMEOUT_SECS)
        for attempt in range(tries):
            state = client.state()
            result = client.call_tool(
                "save_state", {"slot": slot, "description": "past the intro"}
            )
            if result.get("saved"):
                print(json.dumps({
                    "game": game_id, "try": attempt, "room": state.get("room"),
                    "in_control": _in_control(client, state),
                }))
                return True
            if attempt % 10 == 0:
                print(f"  {game_id}: try {attempt}, {result.get('reason')}")
            if attempt < SKIP_TRIES:
                try:
                    client.skip()
                except RuntimeError as error:
                    if "nothing to skip" not in str(error):
                        print(f"  {game_id}: skip: {error}")
            time.sleep(settle)
        print(f"{game_id}: never became saveable after {tries} tries", file=sys.stderr)
        return False
    finally:
        proc.kill()
        proc.wait(timeout=10)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("games", nargs="*", help="game ids to capture")
    parser.add_argument("--all", action="store_true",
                        help="every game with a configured data folder")
    parser.add_argument("--slot", type=int, default=1)
    parser.add_argument("--port", type=int, default=25999)
    parser.add_argument("--tries", type=int, default=DEFAULT_TRIES)
    parser.add_argument("--settle", type=float, default=DEFAULT_SETTLE_SECS)
    args = parser.parse_args(argv)

    games = sorted(GAME_PATHS) if args.all else args.games
    if not games:
        parser.error("name at least one game, or pass --all")

    failed = []
    for game_id in games:
        # Each capture writes into save_slots/<game_id>, so make it first.
        os.makedirs(os.path.dirname(save_slot_path(game_id, args.slot)), exist_ok=True)
        if not capture(game_id, args.slot, args.port, args.tries, args.settle):
            failed.append(game_id)
            # Leave nothing half-written behind: an empty folder would tell the
            # launcher a save exists.
            folder = os.path.dirname(save_slot_path(game_id, args.slot))
            if os.path.isdir(folder) and not os.listdir(folder):
                shutil.rmtree(folder)
    if failed:
        print(f"no save captured for: {', '.join(failed)}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
