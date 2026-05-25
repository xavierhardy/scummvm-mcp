"""
Integration tests for Sam & Max Hit the Road (DOS/CD demo, SCUMM V6).

Sam & Max uses the V6 icon verb bar (walk to / look at / use / talk to /
pick up). Conversations work like The Dig's — picture icons rather than text —
and Sam's spoken lines arrive from the charset buffer prefixed by 0xFF-coded
talkie/sound blocks.

The startup sequence is: a skippable intro, then the credits screen (room 77)
which you advance past with repeated skips, landing in the detectives' office
(room 7). From there the hallway (room 8) leads down to the street (room 9)
with Bosco's liquor store and the DeSoto.

Regression focus: the talkie sound-code prefix on every V6 spoken line used to
surface as a separate garbage "message" (a non-breaking space plus a couple of
random bytes). Those fragments must now be dropped, while real lines — e.g. the
"BIFF"/"POW" onomatopoeia of the liquor-store robbery — still come through clean.
"""

import pytest
from time import sleep

from utils import McpClient, find_object_by_name, find_object_with_verb


INTRO_POLL_SECS = 0.5
INTRO_MAX_SKIPS = 12


def _reach_office(client: McpClient) -> dict:
    """Skip the intro + credits and return the office (room 7) state."""
    for _ in range(INTRO_MAX_SKIPS):
        state = client.state()
        if state.get("room", {}).get("id") == 7 and state.get("objects"):
            return state
        sleep(INTRO_POLL_SECS)
        try:
            client.skip()
        except Exception:
            pass
    sleep(2)
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


def _has_letters(text: str, n: int = 1) -> bool:
    """True if *text* contains at least *n* ASCII letters."""
    return sum(1 for c in text if c.isascii() and c.isalpha()) >= n


def _assert_no_garbage(messages: list) -> None:
    """Every emitted message must be readable text, not a sound-code fragment.

    The talkie prefix decodes (via the game code page) to a non-breaking space
    (U+00A0) plus stray bytes. A clean line always carries real letters.
    """
    for m in messages:
        t = m.get("text", "")
        assert " " not in t, f"talkie-code garbage leaked into a message: {t!r}"
        assert _has_letters(t), f"message has no readable letters: {t!r}"


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_samnmax_initial_state(samnmax_client: McpClient) -> None:
    """Skip startup screens then verify we reach the office."""
    state = _reach_office(samnmax_client)
    assert state.get("room") is not None
    assert state["room"].get("id") == 7, f"Expected office room 7, got {state['room']}"


def test_02_samnmax_v6_verbs_exposed(samnmax_client: McpClient) -> None:
    """V6 icon actions must be translated to MCP verbs."""
    state = _reach_office(samnmax_client)
    verbs = set(state.get("verbs", []))
    expected = {"walk to", "look at", "use", "talk to", "pick up"}
    missing = expected - verbs
    assert not missing, f"Missing expected V6 verbs: {missing}, got: {sorted(verbs)}"


def test_03_samnmax_max_available(samnmax_client: McpClient) -> None:
    """Max must be addressable either as object or inventory item."""
    state = _reach_office(samnmax_client)
    in_objects = find_object_by_name(state, "max") is not None
    in_inventory = any("max" in i.lower() for i in state.get("inventory", []))
    assert in_objects or in_inventory, (
        f"Max not found; objects={[o['name'] for o in state.get('objects', [])]}, "
        f"inventory={state.get('inventory', [])}"
    )


def test_04_samnmax_messages_are_clean(samnmax_client: McpClient) -> None:
    """Interacting with office objects must never surface talkie-code garbage.

    Regression for the V6 sound-code message bug: Sam's office reactions are
    voice cues that used to emit a garbage fragment per line. After the fix they
    either carry real text or are dropped — but never leak the 0xFF prefix.
    """
    _reach_office(samnmax_client)
    state = samnmax_client.state()
    targets = [
        o["name"] for o in state.get("objects", [])
        if "look at" in o.get("compatible_verbs", []) and not o.get("pathway")
    ][:4]
    if not targets:
        pytest.skip("No look-at objects in the office")
    for name in targets:
        try:
            result = samnmax_client.act("look_at", name)
        except RuntimeError:
            continue
        _assert_no_garbage(result.get("messages", []))


def test_05_samnmax_talk_to_max(samnmax_client: McpClient) -> None:
    """Talking to Max produces clean output (and an icon dialog where present)."""
    _reach_office(samnmax_client)
    state = samnmax_client.state()
    max_name = find_object_by_name(state, "max")
    if max_name is None:
        for item in state.get("inventory", []):
            if "max" in item.lower():
                max_name = item
                break
    if max_name is None:
        pytest.skip("Max not found in objects/inventory")

    try:
        result = _act_retry(samnmax_client, "talk_to", max_name, attempts=10)
    except AssertionError:
        pytest.skip("Game stayed in a cutscene; talk_to unavailable in this demo state")
    _assert_no_garbage(result.get("messages", []))

    # If the conversation opens, choices come through as icon labels (Dig-style).
    question = result.get("question") or samnmax_client.state().get("question")
    if question is not None:
        choices = question.get("choices", [])
        assert len(choices) >= 1, f"Expected at least one dialog choice, got: {choices}"
        for choice in choices:
            assert choice.get("label"), f"Empty label for choice: {choice}"
        # Leave the conversation cleanly.
        for _ in range(8):
            q = samnmax_client.state().get("question")
            if not q:
                break
            samnmax_client.answer(len(q["choices"]))


def test_06_samnmax_real_text_capture_clean(samnmax_client: McpClient) -> None:
    """Real V6 spoken text must survive the talkie-code strip intact.

    Drives down to the street (office -> hallway -> street) and pokes the
    liquor-store robbery, whose "BIFF/POW/DUFF" onomatopoeia are genuine text
    lines. They must arrive as clean words with the sound-code prefix removed.
    """
    _reach_office(samnmax_client)
    # Clear any lingering cutscene left by earlier session-scoped tests.
    for _ in range(8):
        try:
            samnmax_client.walk(150, 150)
            break
        except RuntimeError:
            try:
                samnmax_client.skip()
            except Exception:
                pass
            sleep(1.0)

    def _go(verb: str, target) -> None:
        try:
            _act_retry(samnmax_client, verb, target, attempts=4)
        except Exception:
            pass
        sleep(1.0)

    # Office -> hallway (door 62) -> street (stairwell object 82).
    for _ in range(4):
        if samnmax_client.state().get("room", {}).get("id") == 9:
            break
        _go("use", 62)
        _go("walk_to", 82)
        _go("use", 82)

    if samnmax_client.state().get("room", {}).get("id") != 9:
        pytest.skip("Could not reach the street in this demo build")

    store = find_object_by_name(samnmax_client.state(), "liquor_store")
    if store is None:
        pytest.skip("liquor store not present")

    result = samnmax_client.act("use", store)
    msgs = result.get("messages", [])
    _assert_no_garbage(msgs)
    blob = " ".join(m.get("text", "").upper() for m in msgs)
    assert any(w in blob for w in ("BIFF", "POW", "DUFF", "ZAP", "BAM")), (
        f"Expected readable robbery onomatopoeia, got: {[m.get('text') for m in msgs]}"
    )
