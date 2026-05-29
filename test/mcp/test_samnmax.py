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

import pytest
from time import sleep

from utils import McpClient, find_object_by_name


SETTLE_SECS = 0.5
OFFICE_ROOM = 7


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


def _open_max_conversation(client: McpClient) -> dict:
    """Talk to Max and return the pending dialog question (with icon choices)."""
    _office_state(client)
    result = _act_retry(client, "talk_to", "max", attempts=10)
    question = result.get("question") or client.state().get("question")
    return question


def _close_conversation(client: McpClient) -> None:
    """Leave any open conversation via the 'goodbye' topic (the 4th icon)."""
    for _ in range(10):
        q = client.state().get("question")
        if not q:
            return
        choices = q["choices"]
        idx = 4 if len(choices) >= 4 else len(choices)
        try:
            client.answer(idx)
        except RuntimeError:
            pass
        sleep(0.8)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_samnmax_initial_state(samnmax_client: McpClient) -> None:
    """The save slot drops us straight into the office (room 7)."""
    state = _office_state(samnmax_client)
    assert state.get("room") is not None
    assert state["room"].get("id") == OFFICE_ROOM, f"Expected office room 7, got {state['room']}"


def test_02_samnmax_v6_verbs_exposed(samnmax_client: McpClient) -> None:
    """V6 icon actions must be translated to MCP verbs."""
    state = _office_state(samnmax_client)
    verbs = set(state.get("verbs", []))
    expected = {"walk to", "look at", "use", "talk to", "pick up"}
    missing = expected - verbs
    assert not missing, f"Missing expected V6 verbs: {missing}, got: {sorted(verbs)}"


def test_03_samnmax_max_available(samnmax_client: McpClient) -> None:
    """Max must be addressable as a scene actor named 'max'."""
    state = _office_state(samnmax_client)
    assert find_object_by_name(state, "max") is not None, (
        f"Max not found; objects={[o['name'] for o in state.get('objects', [])]}"
    )


def test_04_samnmax_messages_are_clean(samnmax_client: McpClient) -> None:
    """Interacting with office objects must never surface talkie-code garbage."""
    _office_state(samnmax_client)
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


def test_05_samnmax_talk_to_max_opens_dialog(samnmax_client: McpClient) -> None:
    """Talking to Max opens an icon dialog (Dig-style picture topics)."""
    question = _open_max_conversation(samnmax_client)
    try:
        assert question is not None, "talking to Max should open an icon dialog"
        choices = question.get("choices", [])
        assert len(choices) >= 4, f"expected the office topic icons, got: {choices}"
        for choice in choices:
            assert choice.get("label"), f"empty label for choice: {choice}"
    finally:
        _close_conversation(samnmax_client)


def test_06_samnmax_dialog_topic_speaks_exact_lines(samnmax_client: McpClient) -> None:
    """Selecting a topic must make Sam & Max speak the matching lines.

    The first office topic is Sam's "Are you as confused as I am?", answered by
    Max's "Moreso." — a direct regression check that the verb-cycle talk dispatch
    and the icon `answer()` click reach the real conversation script.
    """
    question = _open_max_conversation(samnmax_client)
    if question is None:
        pytest.skip("conversation did not open in this build")
    try:
        result = samnmax_client.answer(1)
        texts = [m.get("text", "") for m in result.get("messages", [])]
        _assert_no_garbage(result.get("messages", []))
        blob = " ".join(texts)
        assert "Are you as confused as I am?" in blob, f"got: {texts}"
        assert "Moreso." in blob, f"got: {texts}"
    finally:
        _close_conversation(samnmax_client)


def test_07_samnmax_dialog_goodbye_closes(samnmax_client: McpClient) -> None:
    """The 'goodbye' topic (the waving-hand icon) ends the conversation.

    On a fresh conversation Sam bows out with "Never mind."; once topics have
    been discussed it becomes "Well, that's all." — either ends the dialog.
    """
    question = _open_max_conversation(samnmax_client)
    if question is None:
        pytest.skip("conversation did not open in this build")

    # The 4th icon (the waving hand) is the goodbye topic.
    result = samnmax_client.answer(4)
    texts = [m.get("text", "") for m in result.get("messages", [])]
    assert any(p in t.lower() for t in texts for p in ("never mind", "that's all")), (
        f"expected a goodbye line, got: {texts}"
    )

    # After the goodbye, no dialog must remain pending.
    for _ in range(6):
        if samnmax_client.state().get("question") is None:
            break
        sleep(0.5)
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
    if question is None:
        pytest.skip("conversation did not open in this build")
    try:
        # The fresh menu shows the four base topics.
        assert len(question["choices"]) == 4, f"expected 4 base topics, got: {question['choices']}"

        # The first selection reveals the fifth topic icon.
        result = samnmax_client.answer(2)
        _assert_no_garbage(result.get("messages", []))
        follow_up = result.get("question") or samnmax_client.state().get("question")
        assert follow_up is not None, "conversation closed unexpectedly"
        labels = [c.get("label") for c in follow_up["choices"]]
        assert len(labels) == 5, f"the fifth topic should appear after a selection, got: {labels}"
        assert len(set(labels)) == 5, f"duplicate choice in: {labels}"

        # Selecting the fifth topic must reach the real script, not click a blank.
        result = samnmax_client.answer(5)
        texts = [m.get("text", "") for m in result.get("messages", [])]
        _assert_no_garbage(result.get("messages", []))
        assert texts, "selecting the fifth topic produced no spoken line (click missed)"
        assert "coffee achiever" in " ".join(texts).lower(), f"got: {texts}"
    finally:
        _close_conversation(samnmax_client)
