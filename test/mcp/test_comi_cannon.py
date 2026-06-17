"""Integration tests for the COMI cannon minigame (save slot 4)."""

import pytest

from utils import McpClient


# ---------------------------------------------------------------------------
# Cannon geometry — 640×480 screen coordinates.
#
# The cannon room shows skeleton boats sailing left across the upper portion
# of the screen and a fort visible in the background (upper-left and sides).
# Coordinates below were validated against the live game by probing the grid.
#
# Fort zone: x ≤ 200 at y ≤ 225, or x ≥ 540 at y ≤ 200.  Any shot in this
#   area triggers an apology from Guybrush with no object state change.
# Boat zone: y = 250–330 across the screen width.  Each hit produces an
#   object state change (old_state=0 → new_state=1) for the struck boat.
# ---------------------------------------------------------------------------

FORT_X, FORT_Y = 200, 175  # reliable fort hit: "Watch where you're shootin'!"

# Probe-validated boat positions (produce object state changes, no messages).
# Additional sweep coordinates follow so the full minigame can be completed
# even if the boats have moved slightly between shots.
BOAT_SWEEP = [
    (100, 325), (200, 300), (320, 275), (100, 300),  # probe-validated
    (150, 310), (250, 290), (350, 280), (450, 265),  # second pass
    (50,  320), (300, 295), (200, 280), (400, 300),  # third pass
]


# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------

def _shoot(client: McpClient, x: int, y: int) -> dict:
    """Fire the cannon at screen position (x, y) and return state changes."""
    payload = {
        "jsonrpc": "2.0",
        "id": client._next_id(),
        "method": "tools/call",
        "params": {"name": "shoot_cannon", "arguments": {"x": x, "y": y}},
    }
    headers = client._headers({"Accept": "text/event-stream"})
    with client._client.stream("POST", client._url, json=payload, headers=headers) as resp:
        return client._decode_stream_response(resp=resp, tool="shoot_cannon")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_01_cannon_room_reachable(comi_s4_client: McpClient) -> None:
    """Save slot 4 must load directly into the cannon minigame room."""
    state = comi_s4_client.state()
    assert state.get("room") is not None, f"expected a room in state, got: {state}"
    assert state["room"]["id"] != 0, (
        f"expected the cannon minigame room, got room 0 (not loaded): {state['room']}"
    )


def test_02_cannon_shoot_returns_result(comi_s4_client: McpClient) -> None:
    """Firing the cannon must return a result without timing out."""
    # Shoot into open sea — no hit expected, but the tool must complete cleanly.
    result = _shoot(comi_s4_client, 320, 215)
    assert result is not None, "shooting the cannon into open sea timed out / returned no result"


def test_03_cannon_shoot_fort_produces_apology(comi_s4_client: McpClient) -> None:
    """Hitting the fort must produce speech — Guybrush apologises to the enemy."""
    result = _shoot(comi_s4_client, FORT_X, FORT_Y)
    assert result is not None, "shooting the cannon at the fort timed out / returned no result"
    messages = [m["text"] for m in result.get("messages", [])]
    assert len(messages) > 0, (
        f"Expected apology speech after hitting the fort, got no messages "
        f"(full result: {result})"
    )


def test_04_cannon_shoot_all_boats(comi_s4_client: McpClient) -> None:
    """Sweeping the boat lane must sink all skeleton boats (one state change each)."""
    destroyed: set[str] = set()

    for x, y in BOAT_SWEEP:
        if len(destroyed) >= 4:
            break
        result = _shoot(comi_s4_client, x, y)
        for change in result.get("objects_changed", []):
            if change.get("old_state") == 0 and change.get("new_state") == 1:
                destroyed.add(change["name"])

    assert len(destroyed) >= 4, (
        f"Expected to sink all boats; only sank {len(destroyed)}: {destroyed}"
    )
