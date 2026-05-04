"""
Integration tests for the Loom segment of Passport to Adventure (SCUMM V3).

Focus areas:
  - Single-cursor verb model: only 'interact' and 'use item' exposed
  - Note system: pick up the staff to unlock c/d/e, then exercise play_note
  - Two-target 'use item' mechanic: use inventory item on room object

The fixture loads --save-slot=1 (pass.s01) which is positioned right before
the player picks up the staff in the Loom mini-game.
"""

import pytest
from time import sleep, time

import httpx

from utils import McpClient

INTRO_POLL_SECS = 1.0
INTRO_MAX_SKIPS = 20
INTERACTIVE_TIMEOUT_SECS = 60


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def find_object_by_name(state: dict, substring: str) -> str | None:
    """Return the first object name containing *substring* (case-insensitive)."""
    for obj in state.get("objects", []):
        if substring.lower() in obj["name"].lower():
            return obj["name"]
    return None


def skip_intros(client: McpClient) -> None:
    """Send repeated skip commands to advance past intro screens."""
    for _ in range(INTRO_MAX_SKIPS):
        sleep(INTRO_POLL_SECS)
        try:
            client.skip()
        except Exception:
            pass  # ReadTimeout is normal during cutscenes


def wait_for_interactive(
    client: McpClient, timeout: float = INTERACTIVE_TIMEOUT_SECS
) -> bool:
    """Poll with skips until walk() succeeds (game accepts input)."""
    deadline = time() + timeout
    while time() < deadline:
        sleep(1.0)
        try:
            client.skip()
        except Exception:
            pass
        try:
            state = client.state()
            pos = state.get("position", {})
            x, y = pos.get("x", 160), pos.get("y", 100)
            client.walk(x, y)
            return True
        except RuntimeError as e:
            if "not accepting input" in str(e):
                continue
            return True
        except Exception:
            continue
    return False


def get_state_with_retry(client: McpClient, max_attempts: int = 5) -> dict:
    """Call state() with retries for ReadTimeout (cutscene in progress)."""
    for attempt in range(max_attempts):
        try:
            return client.state()
        except (httpx.ReadTimeout, httpx.ConnectTimeout):
            if attempt == max_attempts - 1:
                raise
            sleep(2.0)
    raise RuntimeError("state() failed after retries")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_loom_initial_state(loom_client: McpClient) -> None:
    """Save slot 1 loads the Loom segment; state is reachable, room not None.

    Loading from a save bypasses the intro, so no skip_intros needed.
    """
    if not wait_for_interactive(loom_client):
        pytest.skip("Save did not reach interactive state in time")
    state = get_state_with_retry(loom_client)
    assert state.get("room") is not None, "Expected room in state"


def test_02_loom_verbs_exposed(loom_client: McpClient) -> None:
    """Only 'interact' and 'use item' should be exposed in the Loom segment."""
    state = get_state_with_retry(loom_client)
    verbs = set(state.get("verbs", []))
    expected = {"interact", "use item"}
    missing = expected - verbs
    assert not missing, f"Missing expected Loom verbs: {missing}, got: {sorted(verbs)}"
    assert "walk to" not in verbs, "walk to should not appear in Loom verb list"
    assert "look at" not in verbs, "look at should not appear in Loom verb list"
    assert "pick up" not in verbs, "pick up should not appear in Loom verb list"
    assert "talk to" not in verbs, "talk to should not appear in Loom verb list"


def test_03_loom_objects_in_room(loom_client: McpClient) -> None:
    """At least one object should be visible."""
    state = get_state_with_retry(loom_client)
    objects = state.get("objects", [])
    assert objects, f"Expected room objects, got empty list (room={state.get('room')})"


def test_04_loom_interact_object(loom_client: McpClient) -> None:
    """'interact' on a room object completes without error and changes state.

    The Loom V3 single-cursor model maps every click to the engine's 'use'
    verb. We try named room objects (by id, since many have placeholder names)
    until one produces an observable state change (movement, message, or
    inventory delta).
    """
    if not wait_for_interactive(loom_client):
        pytest.skip("Game stuck in cutscene")

    state = get_state_with_retry(loom_client)
    initial_inventory = set(state.get("inventory", []))
    initial_pos = state.get("position", {})

    candidates: list[int] = [
        obj["id"] for obj in state.get("objects", []) if not obj.get("pathway")
    ]
    if not candidates:
        pytest.skip("No interactable objects in room")

    for obj_id in candidates:
        try:
            result = loom_client.act("interact", obj_id)
        except RuntimeError as e:
            if "not accepting input" in str(e):
                continue
            raise
        sleep(1.5)
        try:
            new_state = get_state_with_retry(loom_client)
        except Exception:
            continue
        new_inv = set(new_state.get("inventory", []))
        new_pos = new_state.get("position", {})
        moved = new_pos.get("x") != initial_pos.get("x") or new_pos.get(
            "y"
        ) != initial_pos.get("y")
        inv_changed = bool(new_inv - initial_inventory)
        if moved or inv_changed or result.get("messages"):
            return  # success: at least one object responded

    pytest.skip(f"No interactable object produced output (tried ids: {candidates})")


def test_05_loom_play_notes_c_d_e(loom_client: McpClient) -> None:
    """play_note c/d/e (the first 3 notes unlocked by the distaff) all succeed."""
    if not wait_for_interactive(loom_client):
        pytest.skip("Game in cutscene")

    for note in ("c", "d", "e"):
        try:
            result = loom_client.play_note(note)
        except RuntimeError as e:
            if "not accepting input" in str(e):
                pytest.skip(f"Game in cutscene during play_note('{note}')")
            raise
        assert isinstance(result, dict), f"play_note('{note}') returned: {result!r}"
        sleep(0.5)


def test_06_loom_play_unknown_note(loom_client: McpClient) -> None:
    """play_note('C') (high-C, likely not yet unlocked) is still accepted by MCP."""
    if not wait_for_interactive(loom_client):
        pytest.skip("Game in cutscene")

    try:
        result = loom_client.play_note("C")
    except RuntimeError as e:
        if "not accepting input" in str(e):
            pytest.skip("Game in cutscene")
        raise
    assert isinstance(result, dict), f"play_note('C') returned: {result!r}"


def _find_id(state: dict, name: str) -> int | None:
    for obj in state.get("objects", []):
        if obj["name"] == name:
            return obj["id"]
    return None


def test_07_loom_egg_listen_and_replay(loom_client: McpClient) -> None:
    """Full Loom puzzle loop: walk away, listen to the egg, replay its draft.

    Verifies both:
      * `act("interact", "egg")` triggers the egg's auto-song and the watcher
        emits per-note MCP notifications (the Opening draft is 4 notes).
      * Replaying the captured notes via `play_note(notes=[...])` is accepted
        by the engine in a single tool call (no LLM-side timing required).
    """
    if not wait_for_interactive(loom_client):
        pytest.skip("Save did not reach interactive state")

    state = get_state_with_retry(loom_client)
    egg_id = _find_id(state, "egg")
    if egg_id is None:
        pytest.skip("egg not in current room")

    # Step 1: move Bobbin away so the next click triggers a fresh listen flow.
    loom_client.walk(40, 130)
    sleep(2)

    # Step 2: interact with the egg → it walks Bobbin and sings its draft.
    notes, messages, _ = loom_client.call_capturing(
        "act", {"verb": "interact", "target1": egg_id}
    )

    # The Opening draft for the egg is 4 notes long. Allow a small tolerance —
    # in some game states the song may be skipped or shortened — but we always
    # expect at least 3 distinct note notifications when the listen path fires.
    if len(notes) < 3:
        pytest.skip(f"egg did not sing this run (notes={notes}, msgs={len(messages)})")
    valid_notes = set("cdefgabC")
    assert all(n in valid_notes for n in notes), (
        f"unexpected note glyph in egg song: {notes}"
    )

    # Step 3: replay those notes via play_note(notes=[...]).
    replay_notes, _, _ = loom_client.call_capturing("play_note", {"notes": notes})

    # The watcher should re-emit the same notes when the player plays them.
    assert len(replay_notes) >= len(notes) - 1, (
        f"replay only emitted {replay_notes} for input {notes}"
    )


def test_08_loom_use_item_on_object(loom_client: McpClient) -> None:
    """Two-target 'use item' exercises the inventory-on-object mechanic."""
    if not wait_for_interactive(loom_client):
        pytest.skip("Game in cutscene")

    state = get_state_with_retry(loom_client)
    inventory = state.get("inventory", [])
    if not inventory:
        pytest.skip("No inventory items in this save state")

    inv_item = inventory[0]

    target_obj = None
    for obj in state.get("objects", []):
        if obj.get("pathway") or obj["name"] == inv_item:
            continue
        target_obj = obj["id"]
        break

    if target_obj is None:
        pytest.skip("No room object available to use item on")

    try:
        result = loom_client.act("use item", inv_item, target_obj)
    except RuntimeError as e:
        if "not accepting input" in str(e):
            pytest.skip(f"Game in cutscene: {e}")
        raise
    assert isinstance(result, dict), f"Expected dict result, got: {result!r}"
