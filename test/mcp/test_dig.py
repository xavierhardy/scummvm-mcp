"""
Integration tests for The Dig (DOS demo, SCUMM V7).

The Dig uses the V7 single-cursor / pie-menu UI model. Only 'interact' and
'use item' are exposed as verbs; the engine's own scene-click input script
decides the action (look_at, talk_to, give, etc.) based on the held inventory
verb cursor and the object class.

The fixture loads dig-demo.s01, which puts the player in canyon room 15 with
Brink and Maggie present and 'look_at' / 'trowel' in inventory.
"""

import pytest

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
    """Interact on an actor opens the V7 dialog with the hero's intro line."""
    result = dig_client.act("interact", "brink")
    msgs = result.get("messages", [])
    assert msgs, (
        f"Expected at least one message after interacting with Brink, got: {result}"
    )
    # Hero (actor 1, internal name "low") says the actor's name.
    assert any("brink" in m["text"].lower() for m in msgs), (
        f"Expected hero to acknowledge Brink, got: {msgs}"
    )
    # The Dig demo uses blast-object icons for dialog choices (no captured
    # labels). Dismiss any pending dialog so subsequent session-scoped tests
    # start in the normal verb script.
    if dig_client.state().get("question"):
        dig_client.answer(1)


def test_06_dig_interact_scenery(dig_client: McpClient) -> None:
    """Interact on the plant scenery makes the hero comment on it."""
    result = dig_client.act("interact", "plant")
    msgs = result.get("messages", [])
    assert msgs, f"Expected a hero comment on the plant, got: {result}"
    assert any(
        "respirating" in m["text"].lower() for m in msgs
    ), f"Expected the plant respirating line, got: {msgs}"


def test_07_dig_use_item_on_scenery(dig_client: McpClient) -> None:
    """Using the trowel on the plant fires the item's verb-3 use-handler."""
    result = dig_client.act("use item", "trowel", "plant")
    msgs = result.get("messages", [])
    assert msgs, f"Expected a hero comment, got: {result}"
    assert any(
        "can't use these things together" in m["text"].lower() for m in msgs
    ), f"Expected the trowel-on-plant refusal, got: {msgs}"


def test_09_dig_use_item_on_actor_female(dig_client: McpClient) -> None:
    """Using the trowel on Maggie produces the gendered female refusal."""
    result = dig_client.act("use item", "trowel", "maggie")
    msgs = result.get("messages", [])
    assert msgs, f"Expected a hero comment, got: {result}"
    assert any(
        "she'd want that" in m["text"].lower() for m in msgs
    ), f"Expected Low to refuse using the trowel on Maggie, got: {msgs}"


def test_10_dig_use_item_on_actor_male(dig_client: McpClient) -> None:
    """Using the trowel on Brink produces the gendered male refusal."""
    result = dig_client.act("use item", "trowel", "brink")
    msgs = result.get("messages", [])
    assert msgs, f"Expected a hero comment, got: {result}"
    assert any(
        "he'd want that" in m["text"].lower() for m in msgs
    ), f"Expected Low to refuse using the trowel on Brink, got: {msgs}"


def test_08_dig_leave_scene(dig_client: McpClient) -> None:
    """Walking ego into the right-side clearing fires Maggie's exit line.

    Object 53 ('clearing' on the right) is the pathway out of room 15. Trying
    to leave alone triggers Maggie's scripted protest ("Easy boys...") rather
    than transitioning the room — but it proves the pathway's verb 13
    entrypoint ran, which the previous MCP routing (doSentence verb 7) never
    reached. Maggie's line is captured via the streaming notifications
    regardless of whether the action ultimately settles within the default
    timeout.
    """
    notes, messages, _ = dig_client.call_capturing(
        "act", {"verb": "interact", "target1": 53}
    )
    actor_lines = [
        m for m in messages
        if m.get("type") == "actor" and m.get("actor") == "maggie"
    ]
    assert any(
        "stick to" in m.get("text", "").lower() for m in actor_lines
    ), f"Expected Maggie's 'stick together' line, got: {messages}"
