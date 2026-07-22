"""Shared helpers for the Flight of the Amazon Queen MCP tests.

Committed helpers must be named ``*_helpers.py`` (``.gitignore`` swallows
``_*.py``). Everything here is specific to FOTAQ's world model: the verb-panel
command machinery, and the hotel opening the slot-1 save starts in.
"""

from __future__ import annotations

import time
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from mcp_client import McpClient

# The panel verb set the queen bridge advertises.
QUEEN_VERBS = {
    "open",
    "close",
    "move",
    "give",
    "use",
    "pick_up",
    "talk_to",
    "look_at",
    "walk_to",
}


def wait_for_can_act(client: McpClient, timeout: float = 60.0) -> dict:
    """Poll ``state`` until the game accepts a new command, then return it."""
    deadline = time.time() + timeout
    last: dict = {}
    while time.time() < deadline:
        last = client.state()
        if last.get("can_act"):
            return last
        time.sleep(0.7)
    return last


def rig_sheet_rope(client: McpClient) -> dict:
    """Solve the hotel room from the slot-1 save: sheets → rope → radiator.

    Returns the result of tying the rope to the radiator. Leaves the room
    escape (climbing down the chute) to the caller.
    """
    client.act("pick_up", "sheets")
    client.act("pick_up", "other_sheet")
    client.act("use", "sheet", "other_sheet")
    return client.act("use", "sheet_rope", "radiator")


def descend_to_basement(client: McpClient) -> dict:
    """Rig the sheet rope and climb down the chute; returns the climb result."""
    rig_sheet_rope(client)
    return client.act("use", "radiator_with_sheet_rope")
