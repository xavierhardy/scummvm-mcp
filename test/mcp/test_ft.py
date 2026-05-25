"""
Integration tests for Full Throttle (DOS demo, SCUMM V7).

Full Throttle uses a verb-coin interface: holding the mouse over a hotspot pops
up three action icons around Ben's head —

  * fist  (grab / punch / operate / use)   -> object verb script 9
  * kick  (boot)                           -> object verb script 5
  * mouth (talk / look / comment)          -> object verb script 8

plus a generic single-click 'interact' (resolved to the object's best available
coin action) and 'use item' (an inventory item on a target).

Unlike The Dig (also V7), Full Throttle objects carry real per-verb entrypoints,
so each object only advertises the coin verbs it actually scripts. The demo drops
Ben in the back-alley dumpster scene (room 10): a sign, the dumpster itself, two
piles of boxes and a door.
"""

import pytest
from time import sleep

from utils import (
    McpClient,
    find_object_by_name,
    skip_intros,
    get_state_with_retry,
)


FT_VERBS = {"fist", "kick", "mouth", "interact", "use item"}


def _ready_state(client: McpClient) -> dict:
    """Skip the intro once (idempotent) and return a readable state in room 10."""
    state = get_state_with_retry(client)
    if state.get("room", {}).get("id") != 10 or not state.get("objects"):
        skip_intros(client, max_skips=14, poll_secs=0.6)
        sleep(2)
        state = get_state_with_retry(client)
    return state


def _act_until_done(client: McpClient, *args, attempts: int = 6) -> dict:
    """Run act(), retrying while the game is mid-cutscene/animation."""
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


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_ft_initial_state(ft_client: McpClient) -> None:
    """Intro auto-advances to the playable dumpster alley (room 10)."""
    state = _ready_state(ft_client)
    assert state.get("room") is not None, "Expected room in state"
    assert state["room"].get("id") == 10, f"Expected room 10, got {state['room']}"


def test_02_ft_verbs_exposed(ft_client: McpClient) -> None:
    """The verb-coin set (fist/kick/mouth) + interact + use item must be exposed."""
    state = _ready_state(ft_client)
    verbs = set(state.get("verbs", []))
    missing = FT_VERBS - verbs
    assert not missing, f"Missing FT verbs: {missing}, got: {sorted(verbs)}"
    # Classic V6 verb-bar labels must not leak through the V7 model.
    for forbidden in ("walk to", "look at", "pick up", "talk to"):
        assert forbidden not in verbs, f"{forbidden!r} should not appear in FT verbs"


def test_03_ft_objects_in_room(ft_client: McpClient) -> None:
    """The dumpster scene exposes its scenery objects."""
    state = _ready_state(ft_client)
    names = {obj["name"] for obj in state.get("objects", [])}
    assert "dumpster" in names, f"dumpster not visible (got {sorted(names)})"
    assert "sign" in names, f"sign not visible (got {sorted(names)})"


def test_04_ft_object_verbs_reflect_entrypoints(ft_client: McpClient) -> None:
    """Per-object compatible verbs must mirror the object's real verb scripts.

    The sign can only be examined ('mouth'); it has no fist/kick handler. Every
    object always offers the generic 'interact' and 'use item'.
    """
    state = _ready_state(ft_client)
    sign = next((o for o in state["objects"] if o["name"] == "sign"), None)
    assert sign is not None, "sign object missing"
    cv = set(sign.get("compatible_verbs", []))
    assert "mouth" in cv, f"sign should support 'mouth', got {cv}"
    assert "interact" in cv and "use item" in cv, f"sign missing generic verbs: {cv}"
    assert "fist" not in cv and "kick" not in cv, (
        f"sign must not advertise fist/kick (no handler), got {cv}"
    )


def test_05_ft_mouth_reads_sign(ft_client: McpClient) -> None:
    """'mouth' on the sign makes Ben read it aloud (verb 8 = look/comment)."""
    _ready_state(ft_client)
    result = _act_until_done(ft_client, "mouth", "sign")
    text = " ".join(m["text"].lower() for m in result.get("messages", []))
    assert "dumpster" in text, f"Expected the sign's dumpster warning, got: {result.get('messages')}"


def test_06_ft_fist_on_boxes(ft_client: McpClient) -> None:
    """'fist' (use/grab) on the boxes yields Ben's refusal (verb 9)."""
    _ready_state(ft_client)
    boxes = find_object_by_name(ft_client.state(), "boxes")
    assert boxes is not None, "boxes object missing"
    result = _act_until_done(ft_client, "fist", boxes)
    text = " ".join(m["text"].lower() for m in result.get("messages", []))
    assert "use for those" in text or "use these" in text, (
        f"Expected a fist/use refusal on the boxes, got: {result.get('messages')}"
    )


def test_07_ft_mouth_on_dumpster(ft_client: McpClient) -> None:
    """'mouth' on the dumpster makes Ben comment on having slept in it."""
    _ready_state(ft_client)
    result = _act_until_done(ft_client, "mouth", "dumpster")
    text = " ".join(m["text"].lower() for m in result.get("messages", []))
    assert "woken up" in text or "worse" in text, (
        f"Expected Ben's dumpster comment, got: {result.get('messages')}"
    )


def test_08_ft_interact_resolves_to_action(ft_client: McpClient) -> None:
    """The generic 'interact' resolves to the object's best coin action.

    The sign only scripts 'mouth', so interact must read it (same as mouth) and
    produce messages, not a no-op.
    """
    _ready_state(ft_client)
    result = _act_until_done(ft_client, "interact", "sign")
    assert result.get("messages"), f"interact on sign produced no output: {result}"
    text = " ".join(m["text"].lower() for m in result["messages"])
    assert "dumpster" in text, f"interact(sign) didn't read the sign: {result['messages']}"


def test_09_ft_messages_have_no_garbage(ft_client: McpClient) -> None:
    """Captured V7 talk lines must be clean — no embedded sound-code fragments.

    Regression: Full Throttle prefixes each spoken line in the charset buffer
    with 0xFF-coded talkie blocks. These once surfaced as separate garbage
    'messages' (e.g. a non-breaking-space + two random bytes). Every emitted
    message must now contain readable alphanumeric text.
    """
    _ready_state(ft_client)
    result = _act_until_done(ft_client, "mouth", "sign")
    msgs = result.get("messages", [])
    assert msgs, "expected sign-reading messages"
    for m in msgs:
        t = m.get("text", "")
        assert any(c.isalnum() and ord(c) < 128 for c in t), (
            f"Garbage message with no ASCII alnum content: {t!r}"
        )


def test_10_ft_use_item_two_targets(ft_client: McpClient) -> None:
    """'use item' accepts two targets without error (inventory permitting)."""
    state = _ready_state(ft_client)
    cursor_names = {"look_at", "look at", "interact", "use", "use_item",
                    "use item", "fist", "kick", "mouth", "boot", "eye"}
    inv = [i for i in state.get("inventory", []) if i.lower() not in cursor_names]
    if not inv:
        pytest.skip("No real inventory items in the dumpster scene")
    target = find_object_by_name(state, "dumpster")
    result = _act_until_done(ft_client, "use item", inv[0], target)
    assert isinstance(result, dict), f"Expected dict result, got: {result!r}"
