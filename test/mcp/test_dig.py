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
    state = dig_client.state()
    assert state.get("room") is not None, "Expected room in state"
    assert (
        state["room"].get("id") == 15
    ), f"Expected canyon room 15, got {state['room']}"


def test_02_dig_verbs_exposed(dig_client: McpClient) -> None:
    """V7 must expose 'interact' and 'use item' (single-cursor model)."""
    state = dig_client.state()
    verbs = set(state.get("verbs", []))
    assert {"interact", "use item"}.issubset(
        verbs
    ), f"Missing expected V7 verbs, got: {sorted(verbs)}"
    # Canonical V6 verbs must not leak through.
    for forbidden in ("walk to", "look at", "pick up", "talk to"):
        assert (
            forbidden not in verbs
        ), f"{forbidden!r} should not appear in Dig verb list"


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
    assert (
        msgs
    ), f"Expected at least one message after interacting with Brink, got: {result}"
    # Hero (actor 1, internal name "low") says the actor's name.
    assert any(
        "brink" in m["text"].lower() for m in msgs
    ), f"Expected hero to acknowledge Brink, got: {msgs}"
    # The conversation opens with one icon per topic. The Dig draws these as
    # picture-icon blast objects, captured and exposed as choices with stable
    # per-icon labels.
    choices = dig_client.state().get("question", {}).get("choices")
    assert choices, f"Expected dialog choices after talking to Brink, got: {choices}"
    assert len(choices) >= 2, f"Expected multiple topic icons, got: {choices}"
    # The icon objects map to semantic labels (?, !, stop hand) instead of
    # opaque icon_<num> placeholders.
    labels = [c.get("label") for c in choices]
    assert labels == [
        "question",
        "exclamation",
        "bye",
    ], f"expected semantic icon labels, got: {labels}"
    # Dismiss so subsequent session-scoped tests start in the normal verb script.
    _close_dialog(dig_client)


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
    t1 = " ".join(m["text"].lower() for m in r1.get("messages", []))
    assert (
        "how are you" in t1
    ), f"Expected the 'how are you' topic, got: {r1.get('messages')}"
    assert any(
        m.get("actor") == "brink" for m in r1.get("messages", [])
    ), f"Expected Brink to respond to topic 1, got: {r1.get('messages')}"
    assert "nothing important" not in t1, "Topic 1 fell through to the cancel line"
    _close_dialog(dig_client)

    # Topic icon 2: "This place is eerie." -> Brink: "...desolate..."
    _open_brink_dialog(dig_client)
    r2 = dig_client.answer(2)
    t2 = " ".join(m["text"].lower() for m in r2.get("messages", []))
    assert "eerie" in t2, f"Expected the 'eerie' topic, got: {r2.get('messages')}"
    assert (
        "desolate" in t2
    ), f"Expected Brink's 'desolate' reply, got: {r2.get('messages')}"
    _close_dialog(dig_client)

    # The two branches must differ — the core of the bug was that they didn't.
    assert t1 != t2, "Topics 1 and 2 produced identical dialog"


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
    assert (
        result is not None
    ), "Leave-scene action errored/timed out instead of completing the cutscene"

    actor_lines = [
        m for m in messages if m.get("type") == "actor" and m.get("actor") == "maggie"
    ]
    assert any(
        "stick to" in m.get("text", "").lower() for m in actor_lines
    ), f"Expected Maggie's 'stick together' line, got: {messages}"

    # The cutscene resolves by moving the team into room 16; the action must
    # stay alive through the whole exchange to observe the transition.
    assert (
        result.get("room_changed") == 16 or dig_client.state()["room"]["id"] == 16
    ), f"Expected transition to room 16 after the cutscene, got: {result}"
