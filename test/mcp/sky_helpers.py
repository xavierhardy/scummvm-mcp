"""Shared helpers for the Beneath a Steel Sky MCP tests.

Committed helpers must be named ``*_helpers.py`` (``.gitignore`` swallows
``_*.py``). Everything here is specific to BASS's world model — game
coordinates are compact coordinates (visible area 128..447 x 136..327), and
the icon-bar inventory is driven by the bridge's own click machine.
"""

from __future__ import annotations

import time
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from mcp_client import McpClient

# The fixed verb set the sky bridge advertises.
SKY_VERBS = {"look_at", "interact", "use", "talk_to", "pick_up", "walk_to"}


def wait_for_can_act(client: McpClient, timeout: float = 60.0) -> dict:
    """Poll ``state`` until the game is idle and accepting input, then return it.

    After a save load (and after long scripted sequences, like the walkway
    guard) ``can_act`` can flicker while Foster settles, so also require the
    screen and player position to hold steady across a couple of reads.
    """
    deadline = time.time() + timeout
    prev = None
    stable = 0
    last: dict = {}
    while time.time() < deadline:
        last = client.state()
        key = (
            last.get("room", {}).get("id"),
            last.get("position", {}).get("x"),
            last.get("position", {}).get("y"),
        )
        if last.get("can_act") and key == prev:
            stable += 1
            if stable >= 2:
                return last
        else:
            stable = 0
        prev = key
        time.sleep(0.7)
    return last


def any_floor_point(state: dict) -> tuple[int, int] | None:
    """The centre of the first floor object in ``state``, or ``None``."""
    for obj in state.get("objects", []):
        if obj.get("kind") == "floor":
            x1, y1, x2, y2 = obj["box"]
            return (x1 + x2) // 2, (y1 + y2) // 2
    return None


def wait_for_object(client: McpClient, name: str, timeout: float = 90.0) -> dict:
    """Poll until the game is idle *and* object *name* is on screen.

    Some hotspots only appear once a scripted sequence has fully wound down
    (screen 1's ``exit`` shows up after the guard leaves), and ``can_act``
    alone flickers true during the pauses of such a sequence.
    """
    deadline = time.time() + timeout
    state: dict = {}
    while time.time() < deadline:
        state = wait_for_can_act(client, timeout=max(1.0, deadline - time.time()))
        names = {obj["name"] for obj in state.get("objects", [])}
        if name in names:
            return state
        time.sleep(1.0)
    return state


def activate_joey(client: McpClient) -> dict:
    """Play the opening until Joey is active, returning the last act result.

    Replays the walkthrough's first steps from the slot-1 save (screen 0, top
    of the walkway): grab the loose rung, force the right-hand door with it
    (triggering the guard sequence and the trip to the overhang), come back
    inside, go down the stairs, head right a screen and fit Joey's circuit
    board into the rusty shell found in the junk.
    """
    client.act("pick_up", "rung")
    client.act("use", "metal_bar", "door")  # guard sequence, ends on screen 1
    # Trying the door plays the guard's second beat ("he must've jumped!"),
    # after which the door becomes a usable exit.
    wait_for_object(client, "door")
    client.act("interact", "door")
    wait_for_object(client, "exit")
    client.act("interact", "exit")  # back inside (screen 0, upper walkway)
    wait_for_object(client, "stairs")
    client.act("interact", "stairs")  # down to the lower walkway
    wait_for_object(client, "exit")
    client.act("interact", "exit")  # right a screen (screen 2)
    wait_for_object(client, "junk")
    client.act("look_at", "junk")  # renames the shell junk pile
    result = client.act("use", "circuit_board", "robot_shell")
    wait_for_can_act(client)
    return result
