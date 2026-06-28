"""
Integration tests for the Loom segment of Passport to Adventure (SCUMM V3).

Focus areas:
  - Single-cursor verb model: only 'interact' and 'use item' exposed
  - Note system: pick up the staff to unlock c/d/e, then exercise play_note
  - Two-target 'use item' mechanic: use inventory item on room object

The fixture loads --save-slot=1 (pass.s01) which is positioned right before
the player picks up the staff in the Loom mini-game.
"""

from time import sleep

import pytest

from assertions import assert_messages_contain
from utils import (
    McpClient,
    bind_verb,
    find_id,
    get_state_with_retry,
    require_interactive,
    skip_unless,
)


def _listen_to_egg(client: McpClient):
    """Walk Bobbin away then click the egg so he hears its Opening draft.

    Returns (notes, messages) from the listen, or (None, None) if the egg isn't
    in the room. Self-contained setup: hearing the draft is the prerequisite for
    replaying it to hatch the egg, so tests that replay call this first.
    """
    state = get_state_with_retry(client)
    egg_id = find_id(state, "egg")
    if egg_id is None:
        return None, None
    client.walk(40, 130)
    sleep(2)
    notes, messages, _ = client.call_capturing(
        "act", {"verb": "interact", "target1": egg_id}
    )
    return notes, messages


def _play_note_or_skip(client: McpClient, note) -> dict:
    """Play a single note, skipping the test if the game is mid-cutscene."""
    try:
        return client.play_note(note)
    except RuntimeError as e:
        if "not accepting input" in str(e):
            pytest.skip(f"Game in cutscene during play_note({note!r})")
        raise


def _play_notes(client: McpClient, notes) -> None:
    """Play each note in *notes* in turn, asserting each call returns a dict."""
    for note in notes:
        result = _play_note_or_skip(client, note)
        assert isinstance(result, dict), f"play_note({note!r}) returned: {result!r}"
        sleep(0.5)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_loom_initial_state(loom_client: McpClient) -> None:
    """Save slot 1 loads the Loom segment; state is reachable, room not None.

    Loading from a save bypasses the intro, so no skip_intros needed.
    """
    require_interactive(loom_client, "Save did not reach interactive state in time")
    state = get_state_with_retry(loom_client)
    assert state.get("room") is not None, "Expected room in state"


def test_02_loom_verbs_exposed(loom_client: McpClient) -> None:
    """Only 'interact' and 'use item' should be exposed in the Loom segment."""
    verbs = set(get_state_with_retry(loom_client).get("verbs", []))
    sorted_verbs = sorted(verbs)
    missing = {"interact", "use item"} - verbs
    assert not missing, f"Missing expected Loom verbs: {missing}, got: {sorted_verbs}"
    assert "walk to" not in verbs, "walk to should not appear in Loom verb list"
    assert "look at" not in verbs, "look at should not appear in Loom verb list"
    assert "pick up" not in verbs, "pick up should not appear in Loom verb list"
    assert "talk to" not in verbs, "talk to should not appear in Loom verb list"


def test_03_loom_objects_in_room(loom_client: McpClient) -> None:
    """At least one object should be visible."""
    state = get_state_with_retry(loom_client)
    objects = state.get("objects", [])
    room = state.get("room")
    assert objects, f"Expected room objects, got empty list (room={room})"


def test_04_loom_egg_listen(loom_client: McpClient) -> None:
    """Walk away and click the egg: Bobbin listens to its 4-note draft.

    `act("interact", "egg")` walks Bobbin over and triggers the egg's
    auto-song; the watcher emits per-note MCP notifications. The Opening
    draft is always e-c-e-d. This must run before anything else interacts
    with the room — clicking the egg or the loom first disturbs the listen
    flow (which is why the old object-loop test made this one flaky).
    """
    require_interactive(loom_client, "Save did not reach interactive state")

    state = get_state_with_retry(loom_client)
    objects = state.get("objects")
    assert find_id(state, "egg") is not None, f"egg not in room: {objects}"

    notes, messages = _listen_to_egg(loom_client)
    assert notes == ["e", "c", "e", "d"], f"expected the Opening draft, got {notes}"
    assert_messages_contain(messages, "It's trying to open!")


@pytest.mark.slow
def test_05_loom_egg_replay_hatches(loom_client: McpClient) -> None:
    """Replaying the Opening draft on the distaff hatches the egg.

    `play_note(notes=["e","c","e","d"])` plays the whole draft in one tool
    call. The watcher re-emits each note, then the egg cracks open and the
    full Hetchel-cygnet cutscene plays out (~45 s of dialogue — this test is
    slow because the game is, not the bridge). Afterwards the egg object is
    consumed and gone from the room.
    """
    require_interactive(loom_client, "Save did not reach interactive state")

    # Hearing the egg's draft is the prerequisite for replaying it (self-contained
    # so this runs on its own fresh instance, not after test_04).
    listened, _ = _listen_to_egg(loom_client)
    skip_unless(
        listened == ["e", "c", "e", "d"],
        f"could not hear the Opening draft to replay (got {listened})",
    )

    notes = ["e", "c", "e", "d"]
    replay_notes, messages, result = loom_client.call_capturing(
        "play_note", {"notes": notes}
    )
    assert result is not None, "replay stream errored before the cutscene finished"
    assert replay_notes == notes, f"watcher re-emitted {replay_notes} for {notes}"
    assert_messages_contain(messages, "To follow the swans!")

    state = get_state_with_retry(loom_client)
    assert find_id(state, "egg") is None, "egg should be consumed after hatching"


def test_06_loom_play_notes_c_d_e(loom_client: McpClient) -> None:
    """play_note c/d/e (the first 3 notes unlocked by the distaff) all succeed."""
    require_interactive(loom_client, "Game in cutscene")
    _play_notes(loom_client, ("c", "d", "e"))


def test_07_loom_play_unknown_note(loom_client: McpClient) -> None:
    """play_note('C') (high-C, likely not yet unlocked) is still accepted by MCP."""
    require_interactive(loom_client, "Game in cutscene")
    result = _play_note_or_skip(loom_client, "C")
    assert isinstance(result, dict), f"play_note('C') returned: {result!r}"


def test_08_loom_interact_object(loom_client: McpClient) -> None:
    """'interact' on the loom completes and produces an observable change.

    The Loom V3 single-cursor model maps every click to the engine's 'use'
    verb. The loom is the one deterministic target in the room: Bobbin walks
    over and it sings its own draft. (The old version of this test looped
    over every object in the room — ~50 s of blind clicking that disturbed
    the egg-listen flow; it now runs after the egg tests, on one target.)
    """
    require_interactive(loom_client, "Game stuck in cutscene")

    state = get_state_with_retry(loom_client)
    loom_id = find_id(state, "loom")
    objects = state.get("objects")
    assert loom_id is not None, f"loom not in room: {objects}"
    initial_pos = state.get("position", {})

    interact = bind_verb(loom_client, "interact")
    result = interact(loom_id)
    moved = result.get("position", initial_pos) != initial_pos
    assert moved or result.get("messages"), f"loom interaction did nothing: {result}"
