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

from assertions import (
    assert_actor_spoke,
    assert_message_contains,
    assert_messages_contain,
    assert_no_message_contains,
)
from utils import (
    McpClient,
    bind_verb,
    choice_labels,
    joined_message_text,
    object_names,
)


def _open_brink_dialog(client: McpClient) -> list:
    """Open Brink's conversation and return its choice list.

    Interacting with an actor may briefly leave the game not accepting input
    while the hero walks over and speaks, so retry until the dialog appears.
    """
    for _ in range(10):
        q = client.state().get("question")
        if q:
            return q["choices"]
        try:
            client.act("interact", "brink")
        except RuntimeError:
            pass
        q = client.state().get("question")
        if q:
            return q["choices"]
    raise AssertionError("could not open Brink's dialog")


def _close_dialog(client: McpClient) -> None:
    """Leave any open conversation via its 'bye' icon (the stop hand)."""
    for _ in range(10):
        q = client.state().get("question")
        if not q:
            return
        idx = next(
            (c["id"] for c in q["choices"] if c["label"] == "bye"),
            len(q["choices"]),
        )
        client.answer(idx)
    assert not client.state().get("question"), "dialog did not close"


def test_01_dig_initial_state(dig_client: McpClient) -> None:
    """Save loads cleanly and we can read state with no intro to skip."""
    room = dig_client.state().get("room")
    assert room is not None, "Expected room in state"
    assert room.get("id") == 15, f"Expected canyon room 15, got {room}"


def test_02_dig_verbs_exposed(dig_client: McpClient) -> None:
    """V7 must expose 'interact' and 'use item' (single-cursor model)."""
    verbs = set(dig_client.state().get("verbs", []))
    assert {"interact", "use item"}.issubset(verbs), f"missing: {sorted(verbs)}"
    leaked = {"walk to", "look at", "pick up", "talk to"} & verbs
    assert not leaked, f"canonical V6 verbs leaked into the Dig verb list: {leaked}"


def test_03_dig_objects_in_room(dig_client: McpClient) -> None:
    """Brink, Maggie and at least one scenery object should be visible."""
    names = object_names(dig_client.state())
    assert "brink" in names, f"brink not visible (got {sorted(names)})"
    assert "maggie" in names, f"maggie not visible (got {sorted(names)})"
    assert "platform" in names, f"platform scenery not visible (got {sorted(names)})"


def test_04_dig_inventory(dig_client: McpClient) -> None:
    """The save file ships with the trowel and the look-at cursor in inventory."""
    inv = set(dig_client.state().get("inventory", []))
    assert "trowel" in inv, f"trowel missing from inventory: {inv}"


def test_05_dig_interact_actor(dig_client: McpClient) -> None:
    """Interact on an actor opens the V7 dialog with the hero's intro line."""
    interact = bind_verb(dig_client, "interact")
    result = interact("brink")
    assert result.get("messages"), f"no message after interacting with Brink: {result}"
    # Hero (actor 1, internal name "low") says the actor's name.
    assert_message_contains(result, "brink")
    # The conversation opens with one icon per topic, exposed as choices with
    # stable per-icon semantic labels (?, !, stop hand).
    question = dig_client.state().get("question") or {}
    choices = question.get("choices")
    assert choices, f"expected dialog choices after talking to Brink, got: {choices}"
    assert len(choices) >= 2, f"expected multiple topic icons, got: {choices}"
    labels = choice_labels(question)
    expected_labels = ["question", "exclamation", "bye"]
    assert labels == expected_labels, f"unexpected icon labels: {labels}"
    # Dismiss so subsequent session-scoped tests start in the normal verb script.
    _close_dialog(dig_client)


@pytest.mark.slow
def test_05b_dig_dialog_choices_distinct(dig_client: McpClient) -> None:
    """Each topic icon must dispatch a *different* conversation branch.

    Regression test for the V7 dialog dispatch. The Dig's choices are
    horizontal picture icons whose click target lives only in the blast-object
    queue (room coordinates). The old dispatch set VAR_MOUSE but never the
    room-space VAR_VIRT_MOUSE the dialog script hit-tests, so every answer
    missed the icons and fell through to the hero's "Nothing important." brush
    -off. With the virtual mouse pointed at the icon and a real left click,
    each icon now drives its own branch.
    """
    # Topic icon 1: "How are you doing, Brink?" -> Brink answers.
    _open_brink_dialog(dig_client)
    r1 = dig_client.answer(1)
    assert_message_contains(r1, "how are you")
    assert_actor_spoke(r1, "brink")
    assert_no_message_contains(r1, "nothing important")
    _close_dialog(dig_client)

    # Topic icon 2: "This place is eerie." -> Brink: "...desolate..."
    _open_brink_dialog(dig_client)
    r2 = dig_client.answer(2)
    assert_message_contains(r2, "eerie")
    assert_message_contains(r2, "desolate")
    _close_dialog(dig_client)

    # The two branches must differ — the core of the bug was that they didn't.
    t1, t2 = joined_message_text(r1), joined_message_text(r2)
    assert t1 != t2, "Topics 1 and 2 produced identical dialog"


def test_06_dig_interact_scenery(dig_client: McpClient) -> None:
    """Interact on the plant scenery makes the hero comment on it."""
    interact = bind_verb(dig_client, "interact")
    result = interact("plant")
    assert result.get("messages"), f"no hero comment on the plant: {result}"
    assert_message_contains(result, "respirating")


def test_07_dig_use_item_on_scenery(dig_client: McpClient) -> None:
    """Using the trowel on the plant fires the item's verb-3 use-handler."""
    use_item = bind_verb(dig_client, "use item")
    result = use_item("trowel", "plant")
    assert result.get("messages"), f"expected a hero comment, got: {result}"
    assert_message_contains(result, "can't use these things together")


def test_09_dig_use_item_on_actor_female(dig_client: McpClient) -> None:
    """Using the trowel on Maggie produces the gendered female refusal."""
    use_item = bind_verb(dig_client, "use item")
    result = use_item("trowel", "maggie")
    assert result.get("messages"), f"expected a hero comment, got: {result}"
    assert_message_contains(result, "she'd want that")


def test_10_dig_use_item_on_actor_male(dig_client: McpClient) -> None:
    """Using the trowel on Brink produces the gendered male refusal."""
    use_item = bind_verb(dig_client, "use item")
    result = use_item("trowel", "brink")
    assert result.get("messages"), f"expected a hero comment, got: {result}"
    assert_message_contains(result, "he'd want that")


@pytest.mark.slow
def test_08_dig_leave_scene(dig_client: McpClient) -> None:
    """Leaving room 15 plays a long cutscene that must NOT time out.

    Object 53 ('clearing' on the right) is the pathway out of room 15. Walking
    there triggers a ~minute-long scripted argument (Low/Brink/Maggie debate who
    leads) before the team finally moves to room 16. The dialogue lines come
    seconds apart but the whole exchange runs well past the old 600-frame
    (~20 s) timeout, which previously aborted the action with "action timed
    out" mid-cutscene. With the timeout anchored to the last streamed event for
    V7/V8, each line resets the deadline so the cutscene plays to completion and
    the action settles cleanly with the room transition.
    """
    notes, messages, result = dig_client.call_capturing(
        "act", {"verb": "interact", "target1": 53}
    )
    # A None result means the stream ended in an error (e.g. the old timeout).
    assert result is not None, "leave-scene action errored or timed out"
    assert_messages_contain(messages, "stick to")

    # The cutscene resolves by moving the team into room 16; the action must
    # stay alive through the whole exchange to observe the transition.
    reached_16 = (
        result.get("room_changed") == 16 or dig_client.state()["room"]["id"] == 16
    )
    assert reached_16, f"expected room 16 after the cutscene, got: {result}"
