"""Shared helpers for the Broken Sword 1 MCP tests.

Committed helpers must be named ``*_helpers.py`` (``.gitignore`` swallows
``_*.py``). Everything here is specific to Broken Sword's world model — the
SCUMM-shaped helpers in ``state_helpers`` key off SCUMM-only concepts (e.g.
``_userPut``), which sword1 does not have.
"""

from __future__ import annotations

import time
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from mcp_client import McpClient

# The fixed verb set the sword1 bridge advertises.
SWORD_VERBS = {"look_at", "interact", "use", "talk_to", "pick_up", "walk_to"}


def wait_for_can_act(client: McpClient, timeout: float = 60.0) -> dict:
    """Poll ``state`` until the game is idle and accepting input, then return it.

    ``state.can_act`` is the sword1 analogue of SCUMM's ``_userPut``. But after a
    save load Broken Sword fades in and George may finish a short scripted walk,
    and ``can_act`` flickers true during that window — so also wait for the
    screen and player position to hold steady across a couple of reads, so the
    returned state is a real resting point.
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


def any_floor_point(client: McpClient) -> tuple[int, int] | None:
    """A world point that ``walk`` will accept, discovered from the compact dump.

    Returns the centre of the first ``floor`` compact on the current screen, or
    ``None`` when the debug tool is unavailable or the screen has no floor.
    """
    dump = client.call_tool("debug", {"compacts": True})
    for cpt in dump.get("compacts", []):
        if cpt.get("kind") == "floor" and cpt.get("mouseable"):
            x1, y1, x2, y2 = cpt["box"]
            return (x1 + x2) // 2, (y1 + y2) // 2
    return None
