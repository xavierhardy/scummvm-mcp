"""
Integration tests for Sam & Max Hit the Road (DOS/CD demo, SCUMM V6).

Sam & Max has no verb bar: a right-click cycles a context cursor through the
available verbs (walk / look / pick up / talk / use-Max) and a left-click runs
the current one. The MCP bridge hides that — `act("talk_to", "max")` cycles the
cursor to the "mouth" icon over Max and clicks him, opening a conversation.

Conversations look like The Dig's: a row of picture-icon topics pops up at the
bottom of the screen (drawn as floating objects in the verb strip, not text).
The bridge exposes them as dialog `choices`, and `answer(n)` clicks the n-th
topic. Sam's/Max's spoken replies arrive from the charset buffer.

The fixture loads save slot 1 (samnmax.s01) which drops Sam & Max straight into
the detectives' office (room 7), so there is no intro/credits to skip.

Regression focus:
  * Talking to Max must open the icon dialog and selecting a topic must produce
    the matching spoken lines (the "mouth"/verb-cycle dispatch).
  * The talkie sound-code prefix on every V6 spoken line must not surface as a
    separate garbage "message".
"""

from time import sleep

import pytest
from assertions import (
    assert_message_contains,
    assert_no_message_contains,
    assert_no_talkie_garbage,
)
from utils import (
    McpClient,
    choice_labels,
    find_choice_id,
    find_object_by_name,
    joined_message_text,
    message_texts,
    object_names,
    skip_unless,
)

SETTLE_SECS = 0.5
OFFICE_ROOM = 7
STREET_ROOM = 9


def _office_state(client: McpClient) -> dict:
    """Return the office (room 7) state. The save starts here, so just wait for
    the room to finish loading."""
    for _ in range(10):
        state = client.state()
        if state.get("room", {}).get("id") == OFFICE_ROOM and state.get("objects"):
            return state
        sleep(SETTLE_SECS)
    return client.state()


def _act_retry(client: McpClient, *args, attempts: int = 6):
    """act(), retrying while the game is briefly mid-cutscene/animation."""
    last = None
    for _ in range(attempts):
        try:
            return client.act(*args)
        except RuntimeError as e:
            last = e
            if "not accepting input" in str(e):
                sleep(1.0)
                continue
            raise
    raise AssertionError(f"act{args} never accepted input: {last}")


def _lookable_objects(state: dict, limit: int = 4) -> list:
    """Return up to *limit* non-pathway object names that advertise 'look at'."""
    names = [
        o["name"]
        for o in state.get("objects", [])
        if "look at" in o.get("compatible_verbs", []) and not o.get("pathway")
    ]
    return names[:limit]


def _assert_clean_look_at(client: McpClient, names: list) -> None:
    """look_at each name and assert no talkie-code garbage in the replies."""
    for name in names:
        try:
            result = client.act("look_at", name)
        except RuntimeError:
            continue
        assert_no_talkie_garbage(result.get("messages", []))


def _open_max_conversation(client: McpClient) -> dict:
    """Talk to Max and return the pending dialog question (with icon choices)."""
    _office_state(client)
    result = _act_retry(client, "talk_to", "max", attempts=10)
    question = result.get("question") or client.state().get("question")
    return question


def _close_conversation(client: McpClient) -> None:
    """Leave any open conversation via the 'bye' topic (the waving-hand icon)."""
    for _ in range(10):
        q = client.state().get("question")
        if not q:
            return
        choices = q["choices"]
        idx = next(
            (c["id"] for c in choices if c["label"] == "bye"),
            4 if len(choices) >= 4 else len(choices),
        )
        try:
            client.answer(idx)
        except RuntimeError:
            pass
        sleep(0.8)


def _has_goodbye(result: dict) -> bool:
    """True if *result* carries one of Sam's conversation-ending goodbye lines."""
    blob = joined_message_text(result).lower()
    return "never mind" in blob or "that's all" in blob


def _wait_dialog_closed(client: McpClient, tries: int = 6) -> None:
    """Poll until no dialog question remains pending."""
    for _ in range(tries):
        if client.state().get("question") is None:
            return
        sleep(SETTLE_SECS)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_samnmax_initial_state(samnmax_client: McpClient) -> None:
    """The save slot drops us straight into the office (room 7)."""
    state = _office_state(samnmax_client)
    room = state.get("room")
    assert room is not None, f"expected a room, got {state}"
    assert room.get("id") == OFFICE_ROOM, f"Expected office room 7, got {room}"


def test_02_samnmax_v6_verbs_exposed(samnmax_client: McpClient) -> None:
    """V6 icon actions must be translated to MCP verbs."""
    verbs = set(_office_state(samnmax_client).get("verbs", []))
    expected = {"walk to", "look at", "use", "talk to", "pick up"}
    missing = expected - verbs
    sorted_verbs = sorted(verbs)
    assert not missing, f"Missing expected V6 verbs: {missing}, got: {sorted_verbs}"


def test_03_samnmax_max_available(samnmax_client: McpClient) -> None:
    """Max must be addressable as a scene actor named 'max'."""
    state = _office_state(samnmax_client)
    names = sorted(object_names(state))
    max_obj = find_object_by_name(state, "max")
    assert max_obj is not None, f"Max not found; objects={names}"


def test_04_samnmax_messages_are_clean(samnmax_client: McpClient) -> None:
    """Interacting with office objects must never surface talkie-code garbage."""
    _office_state(samnmax_client)
    targets = _lookable_objects(samnmax_client.state())
    skip_unless(bool(targets), "No look-at objects in the office")
    _assert_clean_look_at(samnmax_client, targets)


def test_05_samnmax_talk_to_max_opens_dialog(samnmax_client: McpClient) -> None:
    """Talking to Max opens an icon dialog (Dig-style picture topics)."""
    question = _open_max_conversation(samnmax_client)
    try:
        assert question is not None, "talking to Max should open an icon dialog"
        choices = question.get("choices", [])
        assert len(choices) >= 4, f"expected the office topic icons, got: {choices}"
        # The icon objects map to semantic labels (?, !, golden duck, waving
        # hand) instead of opaque icon_<num> placeholders.
        labels = choice_labels(question)
        expected = ["question", "exclamation", "tease", "bye"]
        assert labels == expected, f"expected semantic icon labels, got: {labels}"
    finally:
        _close_conversation(samnmax_client)


def test_06_samnmax_dialog_topic_speaks_exact_lines(samnmax_client: McpClient) -> None:
    """Selecting a topic must make Sam & Max speak the matching lines.

    The first office topic is Sam's "Are you as confused as I am?", answered by
    Max's "Moreso." — a direct regression check that the verb-cycle talk dispatch
    and the icon `answer()` click reach the real conversation script.
    """
    question = _open_max_conversation(samnmax_client)
    skip_unless(question is not None, "conversation did not open in this build")
    try:
        result = samnmax_client.answer(1)
        assert_no_talkie_garbage(result.get("messages", []))
        assert_message_contains(result, "Are you as confused as I am?")
        assert_message_contains(result, "Moreso.")
    finally:
        _close_conversation(samnmax_client)


def test_07_samnmax_dialog_goodbye_closes(samnmax_client: McpClient) -> None:
    """The 'goodbye' topic (the waving-hand icon) ends the conversation.

    On a fresh conversation Sam bows out with "Never mind."; once topics have
    been discussed it becomes "Well, that's all." — either ends the dialog.
    """
    question = _open_max_conversation(samnmax_client)
    skip_unless(question is not None, "conversation did not open in this build")

    # The 4th icon (the waving hand) is the goodbye topic.
    result = samnmax_client.answer(4)
    texts = message_texts(result)
    assert _has_goodbye(result), f"expected a goodbye line, got: {texts}"

    # After the goodbye, no dialog must remain pending.
    _wait_dialog_closed(samnmax_client)
    assert samnmax_client.state().get("question") is None, "conversation did not close"


def test_08_samnmax_fifth_topic_appears_and_selects(samnmax_client: McpClient) -> None:
    """A topic revealed mid-conversation must be selectable, not a dead click.

    Once Sam & Max are talking, a fifth office topic (the Max-head icon, object
    1067) appears packed into the next free slot on the left of the strip. The
    engine keeps reporting that icon's dormant home X — parked behind the
    rightmost blank box — so the bridge used to aim answer(5) at the blank on top
    of it and the click silently did nothing. The choice must now appear after
    the first selection and selecting it must produce its spoken exchange.
    """
    question = _open_max_conversation(samnmax_client)
    skip_unless(question is not None, "conversation did not open in this build")
    try:
        # The fresh menu shows the four base topics.
        base_choices = question["choices"]
        assert len(base_choices) == 4, f"expected 4 base topics, got: {base_choices}"

        # The first selection reveals the fifth topic icon.
        result = samnmax_client.answer(2)
        assert_no_talkie_garbage(result.get("messages", []))
        follow_up = result.get("question") or samnmax_client.state().get("question")
        assert follow_up is not None, "conversation closed unexpectedly"
        labels = choice_labels(follow_up)
        assert len(labels) == 5, f"fifth topic should appear: {labels}"
        assert len(set(labels)) == 5, f"duplicate choice in: {labels}"
        # The revealed icon is the Max head (object 1067) — labelled "max".
        assert "max" in labels, f"expected the 'max' topic icon, got: {labels}"

        # Selecting the Max topic must reach the real script, not click a blank.
        max_id = find_choice_id(follow_up, "max")
        result = samnmax_client.answer(max_id)
        texts = message_texts(result)
        assert_no_talkie_garbage(result.get("messages", []))
        assert texts, "selecting the fifth topic produced no spoken line (click missed)"
        assert_message_contains(result, "coffee achiever")
    finally:
        _close_conversation(samnmax_client)


# ---------------------------------------------------------------------------
# Street scene (room 9): icon-verb mapping, phantom objects, vehicle cutscene
# ---------------------------------------------------------------------------


def _room_id(state: dict):
    return (state.get("room") or {}).get("id")


def _wait_room(client: McpClient, target: int, tries: int = 30) -> bool:
    for _ in range(tries):
        if _room_id(client.state()) == target:
            return True
        sleep(SETTLE_SECS)
    return False


def _wait_leave_room(client: McpClient, room: int, tries: int = 30) -> bool:
    """Poll until the client is no longer in *room*; return True if it left."""
    for _ in range(tries):
        if _room_id(client.state()) != room:
            return True
        sleep(SETTLE_SECS)
    return False


def _goto_street(client: McpClient) -> bool:
    """Walk Sam & Max office(7) -> hallway(8) -> street(9).

    The office door (id 62) and the downstairs exit (id 82) both carry a verb-7
    ('use') handler. The downstairs only transitions once Sam is stood by it, so
    we nudge him toward the bottom-left exit before using it. Returns True if the
    street is reached.

    The samnmax fixture is session-scoped, so a prior street test may already have
    walked us here — short-circuit in that case instead of trying to navigate back
    from a non-office room.
    """
    if _room_id(client.state()) == STREET_ROOM:
        return True
    _office_state(client)
    try:
        _act_retry(client, "use", 62)
    except (RuntimeError, AssertionError):
        pass
    if not _wait_room(client, 8):
        return False
    sleep(1.0)
    for _ in range(4):
        try:
            client.walk(50, 180)
        except RuntimeError:
            pass
        sleep(1.5)
        try:
            client.act("use", 82)
        except RuntimeError:
            pass
        if _wait_room(client, STREET_ROOM, tries=12):
            return True
    return _room_id(client.state()) == STREET_ROOM


def test_09_samnmax_look_and_pick_up_not_reversed(samnmax_client: McpClient) -> None:
    """'look at' must examine and 'pick up' must (try to) take — not the reverse.

    Sam & Max's icon verbs invert the common V6 layout: the eye (look) is verb 4
    and the hand (pick up) is verb 5, the opposite of Day of the Tentacle's
    {4:pick_up, 5:look_at}. The bridge used to resolve the names to the V6 ids and
    swapped the two actions. Verified on Max's roach farm in the office, which
    needs no navigation.
    """
    _office_state(samnmax_client)
    look_result = _act_retry(samnmax_client, "look_at", "roach_farm")
    assert_no_talkie_garbage(look_result.get("messages", []))
    assert_message_contains(look_result, "roach farm")
    assert_no_message_contains(look_result, "pick")

    pick_result = _act_retry(samnmax_client, "pick_up", "roach_farm")
    assert_message_contains(pick_result, "pick")
    assert_no_message_contains(pick_result, "roach farm")


def test_10_samnmax_no_phantom_carnival_ticket(samnmax_client: McpClient) -> None:
    """The dormant 'carnival_tickets' object (id 95) must not be a selectable target.

    On the street it sits at 0,0 with a zero-size bounding box until the script
    later places it, so the player can never click it. It used to leak into the
    object list as a phantom, pickable target.
    """
    skip_unless(
        _goto_street(samnmax_client), "could not reach the street scene (room 9)"
    )
    names = object_names(samnmax_client.state())
    sorted_names = sorted(names)
    assert "carnival_tickets" not in names, f"phantom ticket listed: {sorted_names}"
    # The real, placed scenery is still there.
    assert "beat_up_desoto" in names, f"the DeSoto should be listed: {sorted_names}"


@pytest.mark.slow
def test_11_samnmax_use_desoto_triggers_cutscene(samnmax_client: McpClient) -> None:
    """Using the DeSoto must board the car and play the drive-away cutscene.

    The car carries no verb-7 script, so doSentence('use') did nothing. Boarding
    is fired by the scene-click input script when clicked with the use/operate
    context cursor; the bridge now cycles the cursor there and clicks, leaving the
    street (room 9) for the driving cutscene.
    """
    skip_unless(
        _goto_street(samnmax_client), "could not reach the street scene (room 9)"
    )
    room_id = _room_id(samnmax_client.state())
    assert room_id == STREET_ROOM, "expected to be on the street"
    _act_retry(samnmax_client, "use", "beat_up_desoto")
    left = _wait_leave_room(samnmax_client, STREET_ROOM)
    assert left, "using the DeSoto did not trigger the drive-away cutscene"
