"""Integration tests for the COMI cannon minigame (save slot 4).

The four skeleton war-canoes are clusters of actor sprites, not background
objects, so the engine surfaces each as a synthetic ``boat_N`` object in
``state.objects`` carrying the exact screen point to aim at. ``shoot_cannon``
maps that point onto the cannon's barrel column and elevation and fires; the
ball lands at the boat and sinks it. The number of canoes still afloat is
reported back as ``boats_remaining`` (a sunk boat is an actor cluster vanishing,
which never appears in ``objects_changed``), reaching 0 when the minigame is won.
"""

from utils import McpClient


def _shoot(client: McpClient, x: int, y: int) -> dict:
    """Fire the cannon at screen position (x, y) and return state changes."""
    payload = {
        "jsonrpc": "2.0",
        "id": client._next_id(),
        "method": "tools/call",
        "params": {"name": "shoot_cannon", "arguments": {"x": x, "y": y}},
    }
    headers = client._headers({"Accept": "text/event-stream"})
    with client._client.stream(
        "POST", client._url, json=payload, headers=headers
    ) as resp:
        return client._decode_stream_response(resp=resp, tool="shoot_cannon")


def _boats(client: McpClient) -> list[tuple[int, int]]:
    """Return the (x, y) of every boat still afloat, left-to-right."""
    return [
        (o["x"], o["y"])
        for o in client.state().get("objects", [])
        if str(o.get("name", "")).startswith("boat")
    ]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_cannon_room_reachable(comi_s4_client: McpClient) -> None:
    """Save slot 4 must load directly into the cannon minigame room."""
    state = comi_s4_client.state()
    assert state.get("room") is not None, f"expected a room in state, got: {state}"
    assert (
        state["room"]["id"] != 0
    ), f"expected the cannon minigame room, got room 0 (not loaded): {state['room']}"


def test_02_boats_exposed_in_state(comi_s4_client: McpClient) -> None:
    """The four war-canoes must be surfaced as boat_N aim targets in state."""
    boats = _boats(comi_s4_client)
    assert len(boats) == 4, f"expected 4 boats in state, got {len(boats)}: {boats}"
    for x, y in boats:
        assert 0 < x < 640 and 0 < y < 480, f"boat at implausible coords ({x},{y})"


def test_03_cannon_shoot_sinks_targeted_boat(comi_s4_client: McpClient) -> None:
    """Aiming at a boat's own coordinates must sink that boat in one shot."""
    boats = _boats(comi_s4_client)
    assert boats, "no boats to shoot"
    target = boats[0]
    result = _shoot(comi_s4_client, *target)
    assert result is not None, "shooting the cannon timed out / returned no result"
    assert result.get("boats_remaining") == len(boats) - 1, (
        f"expected boats_remaining to drop to {len(boats) - 1} after a clean hit, "
        f"got {result.get('boats_remaining')} (result: {result})"
    )


def test_04_cannon_shoot_all_boats(comi_s4_client: McpClient) -> None:
    """Aiming at each boat in turn must sink all four and win the minigame."""
    last_remaining = None
    for _ in range(8):  # at most a couple of retries beyond the 4 clean shots
        boats = _boats(comi_s4_client)
        if not boats:
            break
        result = _shoot(comi_s4_client, *boats[0])
        last_remaining = result.get("boats_remaining")

    remaining = _boats(comi_s4_client)
    assert not remaining, (
        f"expected to sink all boats; {len(remaining)} still afloat: {remaining} "
        f"(last boats_remaining={last_remaining})"
    )
    assert last_remaining == 0, (
        f"expected the final shot to report boats_remaining=0, got {last_remaining}"
    )
