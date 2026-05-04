"""
Integration tests for The Dig (DOS demo, SCUMM V7).

The Dig uses the V7 single-cursor / pie-menu UI model. Only 'interact' and
'use item' are exposed as verbs; the engine's own scene-click input script
decides the action (look_at, talk_to, give, etc.) based on the held inventory
verb cursor and the object class.

The fixture loads dig-demo.s01, which puts the player in canyon room 15 with
Brink and Maggie present and 'look_at' / 'trowel' in inventory.
"""

from utils import McpClient


def test_01_dig_initial_state(dig_client: McpClient) -> None:
    """Save loads cleanly and we can read state with no intro to skip."""
    state = dig_client.state()
    assert state.get("room") is not None, "Expected room in state"
    assert state["room"].get("id") == 15, (
        f"Expected canyon room 15, got {state['room']}"
    )


def test_02_dig_verbs_exposed(dig_client: McpClient) -> None:
    """V7 must expose 'interact' and 'use item' (single-cursor model)."""
    state = dig_client.state()
    verbs = set(state.get("verbs", []))
    assert {"interact", "use item"}.issubset(verbs), (
        f"Missing expected V7 verbs, got: {sorted(verbs)}"
    )
    # Canonical V6 verbs must not leak through.
    for forbidden in ("walk to", "look at", "pick up", "talk to"):
        assert forbidden not in verbs, (
            f"{forbidden!r} should not appear in Dig verb list"
        )


def test_03_dig_objects_in_room(dig_client: McpClient) -> None:
    """Brink, Maggie and at least one scenery object should be visible."""
    state = dig_client.state()
    names = {obj["name"] for obj in state.get("objects", [])}
    assert "brink" in names, f"brink not visible (got {sorted(names)})"
    assert "maggie" in names, f"maggie not visible (got {sorted(names)})"
    assert "platform" in names, f"platform scenery not visible (got {sorted(names)})"


def test_04_dig_inventory(dig_client: McpClient) -> None:
    """The save file ships with the trowel and the look-at cursor in inventory."""
    inv = set(dig_client.state().get("inventory", []))
    assert "trowel" in inv, f"trowel missing from inventory: {inv}"


def test_05_dig_interact_actor(dig_client: McpClient) -> None:
    """Interact on an actor walks the hero over and produces the look line."""
    result = dig_client.act("interact", "brink")
    msgs = result.get("messages", [])
    assert msgs, (
        f"Expected at least one message after interacting with Brink, got: {result}"
    )
    # Hero (actor 1, internal name "low") says the actor's name.
    assert any("brink" in m["text"].lower() for m in msgs), (
        f"Expected hero to acknowledge Brink, got: {msgs}"
    )


def test_06_dig_interact_object(dig_client: McpClient) -> None:
    """Interact on a scenery object produces a hero comment line."""
    result = dig_client.act("interact", "platform")
    msgs = result.get("messages", [])
    assert msgs, (
        f"Expected at least one message after interacting with platform, got: {result}"
    )


def test_07_dig_use_item_on_actor(dig_client: McpClient) -> None:
    """Using the trowel on Maggie produces a hero refusal line."""
    result = dig_client.act("use item", "trowel", "maggie")
    msgs = result.get("messages", [])
    assert msgs, f"Expected at least one message, got: {result}"
