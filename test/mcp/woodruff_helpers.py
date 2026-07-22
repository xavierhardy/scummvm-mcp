#!/usr/bin/env python3
"""Helpers for the Woodruff (Gob engine) integration test.

Woodruff has no save support, so its test starts a fresh instance and skips the
intro. The engine plays every action out over a stream and, like the SCUMM
demos, may briefly hold input while a walk/pickup animates — so calls are gently
retried on "not accepting input". Object/inventory names come from the game's
own status-bar text (harvested by the bridge), so tests match on those names.
"""

from __future__ import annotations

from time import sleep
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from mcp_client import McpClient

# Rooms, by the trailing number of their TOT file (see mcpGobScreenId).
ROOM_AZIMUTH = 2002  # emap2002 — Azimuth's devastated house (start screen)
ROOM_BOOZOOK = 2003  # emap2003 — the street of the sad Boozook (one screen right)

# Errors that mean the call itself is wrong, not that input is briefly held.
_PERMANENT = ("no such", "unknown verb", "not found", "already in progress")


def _state(client: McpClient) -> dict:
    return client.state()


def room(client: McpClient) -> int | None:
    return (_state(client).get("room") or {}).get("id")


def object_names(client: McpClient) -> list[str]:
    return [o["name"] for o in _state(client).get("objects", [])]


def inventory_names(client: McpClient) -> list[str]:
    return [i["name"] for i in _state(client).get("inventory", [])]


def messages_text(result: dict) -> str:
    return " ".join(m.get("text", "") for m in (result.get("messages") or []))


def act(client: McpClient, verb: str, target1=None, target2=None, attempts: int = 20):
    """act() retried while a walk/cutscene is briefly holding input."""
    last = None
    for _ in range(attempts):
        try:
            return client.act(verb, target1, target2) or {}
        except RuntimeError as exc:
            last = exc
            if any(p in str(exc).lower() for p in _PERMANENT):
                raise
            sleep(1.0)
    raise AssertionError(f"act({verb!r}, {target1!r}, {target2!r}) never ran: {last}")


def skip_intro(client: McpClient, max_skips: int = 30) -> bool:
    """Skip the intro videos until the game is interactive in a real room.

    Each `skip` breaks one intro video; once the game is waiting for input the
    tool rejects the call with "nothing to skip". That alone is not enough — the
    game is briefly idle between videos too — so keep going until the first
    playable screen (a room past the intro) is actually up and interactive."""
    for _ in range(max_skips):
        try:
            client.skip()
        except RuntimeError as exc:
            if "nothing to skip" not in str(exc):
                raise
        sleep(1.0)
        st = _state(client)
        room_id = (st.get("room") or {}).get("id")
        if st.get("can_act") and room_id and room_id >= ROOM_AZIMUTH:
            return True
    return False


def wait_named_objects(client: McpClient, timeout: float = 12.0) -> list[str]:
    """Wait until the bridge has harvested the screen's object names.

    After a room settles the bridge sweeps the hotspots to learn their names;
    `naming_pending` is False and the object list is populated once it is done."""
    for _ in range(int(timeout * 2)):
        st = _state(client)
        objs = [o["name"] for o in st.get("objects", [])]
        if objs and not st.get("naming_pending"):
            return objs
        sleep(0.5)
    return object_names(client)


def wait_room(client: McpClient, room_id: int, tries: int = 20, poll: float = 1.0):
    for _ in range(tries):
        if room(client) == room_id:
            return True
        sleep(poll)
    return room(client) == room_id


def wait_in_inventory(client: McpClient, name: str, tries: int = 20, poll: float = 1.0):
    for _ in range(tries):
        if name in inventory_names(client):
            return True
        sleep(poll)
    return name in inventory_names(client)
