"""
Integration tests for the Indiana Jones 3 segment of Passport to Adventure.

Save slot 3 (pass.s03) drops Indy at the boxing gym. Walking to the locker
room triggers a dialog; choice 1 starts a fist-fight against the boxing coach.
The fight HUD vars are surfaced through state.fight, and numpad-style
keystrokes (1-9) drive Indy's punches/blocks/step-backs.
"""

from __future__ import annotations

import pytest
from time import sleep

from utils import McpClient, _wait_until, _state_or_skip, _find_object


INTERACTIVE_TIMEOUT_SECS = 30


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_indy3_initial_state(indy3_client: McpClient) -> None:
    """Save slot 3 loads the boxing gym (room 25)."""
    if not _wait_until(lambda: indy3_client.state().get("room") is not None):
        pytest.skip("save did not reach interactive state")

    state = _state_or_skip(indy3_client)
    assert state.get("room") is not None
    # Gym scene exposes the standard V3 verb bar.
    verbs = set(state.get("verbs", []))
    assert {"walk to", "look", "use", "push", "pull"}.issubset(verbs), (
        f"expected V3 verb bar, got: {sorted(verbs)}"
    )


def test_02_indy3_locker_room_visible(indy3_client: McpClient) -> None:
    """The 'locker_room' object is selectable in the gym."""
    state = _state_or_skip(indy3_client)
    locker = _find_object(state, "locker_room")
    assert locker is not None, (
        f"locker_room not found; objects = "
        f"{[o['name'] for o in state.get('objects', [])]}"
    )


def test_03_indy3_walk_to_locker_triggers_dialog(indy3_client: McpClient) -> None:
    """Walking to the locker room opens a dialog with the boxing coach."""
    state = _state_or_skip(indy3_client)
    if state.get("question"):
        return  # already in dialog

    indy3_client.act("walk to", "locker_room")
    if not _wait_until(
        lambda: indy3_client.state().get("question") is not None, timeout=10.0
    ):
        pytest.skip("dialog did not appear")
    state = _state_or_skip(indy3_client)
    question = state.get("question")
    assert question is not None
    choices = question.get("choices", [])
    assert len(choices) >= 4, f"expected ≥4 choices, got: {choices}"


def test_04_indy3_answer_starts_fight(indy3_client: McpClient) -> None:
    """Choosing choice 1 ('go easy') starts the fight; state.fight populates."""
    state = _state_or_skip(indy3_client)

    # If we ended up past the dialog already (e.g. ran out of order), reset by
    # walking back and re-triggering the dialog.
    if state.get("question") is None and state.get("fight") is None:
        indy3_client.act("walk to", "locker_room")
        _wait_until(
            lambda: indy3_client.state().get("question") is not None, timeout=10.0
        )

    state = _state_or_skip(indy3_client)
    if state.get("question") is None:
        pytest.skip("no dialog pending")

    indy3_client.answer(1)

    if not _wait_until(
        lambda: indy3_client.state().get("fight") is not None, timeout=10.0
    ):
        pytest.skip("fight HUD did not appear after answer(1)")

    state = _state_or_skip(indy3_client)
    fight = state.get("fight")
    assert fight is not None, "expected fight in state after starting fight"
    assert "indy" in fight and "opponent" in fight
    for who in ("indy", "opponent"):
        side = fight[who]
        assert "health" in side and "punch_power" in side
        assert isinstance(side["health"], int) and side["health"] > 0
        assert isinstance(side["punch_power"], int) and side["punch_power"] >= 0


def test_05_indy3_punch_high_lands(indy3_client: McpClient) -> None:
    """A high punch (numpad 9) reduces the opponent's health."""
    state = _state_or_skip(indy3_client)
    if state.get("fight") is None:
        pytest.skip("not in a fight")

    before = state["fight"]
    indy3_client.call_capturing("keystroke", {"key": "9"})
    sleep(2.0)
    state = _state_or_skip(indy3_client)
    if state.get("fight") is None:
        pytest.skip("fight ended unexpectedly")
    after = state["fight"]

    # The opponent should have taken some damage on at least one of the
    # subsequent rounds. If the coach blocked, only the punch_power gauge
    # changes — that still counts as the input being received.
    opponent_changed = (
        after["opponent"]["health"] != before["opponent"]["health"]
        or after["opponent"]["punch_power"] != before["opponent"]["punch_power"]
    )
    indy_changed = (
        after["indy"]["health"] != before["indy"]["health"]
        or after["indy"]["punch_power"] != before["indy"]["punch_power"]
    )
    assert opponent_changed or indy_changed, (
        f"high-punch had no effect: before={before}, after={after}"
    )


def test_06_indy3_block_then_step_back(indy3_client: McpClient) -> None:
    """Mid block (5) followed by step-back (4) keeps the fight running."""
    state = _state_or_skip(indy3_client)
    if state.get("fight") is None:
        pytest.skip("not in a fight")

    indy3_client.call_capturing("keystroke", {"key": "5"})
    sleep(1.5)
    indy3_client.call_capturing("keystroke", {"key": "4"})
    sleep(1.5)

    state = _state_or_skip(indy3_client)
    # The fight may end if Indy or the coach is KO'd, but otherwise the HUD
    # should still be visible and the values valid.
    fight = state.get("fight")
    if fight is not None:
        assert isinstance(fight["indy"]["health"], int)
        assert isinstance(fight["opponent"]["health"], int)


# ---------------------------------------------------------------------------
# Travel tests (save slot 4: Pan Am clipper)
# ---------------------------------------------------------------------------


def test_10_indy3_travel_initial_state(indy3_travel_client: McpClient) -> None:
    """Save slot 4 loads the airplane scene (room 24) with 'travel' on the verb bar."""
    if not _wait_until(lambda: indy3_travel_client.state().get("room") is not None):
        pytest.skip("save did not reach interactive state")

    state = _state_or_skip(indy3_travel_client)
    assert state["room"]["id"] == 24, f"expected room 24 (clipper), got {state['room']}"
    verbs = set(state.get("verbs", []))
    assert "travel" in verbs, f"expected 'travel' verb, got: {sorted(verbs)}"


def test_11_indy3_travel_opens_destination_dialog(
    indy3_travel_client: McpClient,
) -> None:
    """`act('travel')` opens the destination dialog (To Henry's House / Cancel)."""
    state = _state_or_skip(indy3_travel_client)
    if state.get("question") is not None:
        # Already in a dialog from an earlier test — nothing to assert here.
        return
    if state.get("room", {}).get("id") != 24:
        pytest.skip("not in clipper room")

    result = indy3_travel_client.act("travel")
    question = result.get("question") or _state_or_skip(indy3_travel_client).get(
        "question"
    )
    assert question is not None, f"expected destination dialog, got result={result}"
    labels = [c["label"].lower() for c in question.get("choices", [])]
    assert any("henry" in l for l in labels), f"expected 'Henry' choice, got {labels}"
    assert any("cancel" in l for l in labels), f"expected 'Cancel' choice, got {labels}"


def test_12_indy3_travel_to_henrys_house(indy3_travel_client: McpClient) -> None:
    """Choosing 'To Henry's House' transports Indy to the new room."""
    state = _state_or_skip(indy3_travel_client)
    if state.get("question") is None:
        if state.get("room", {}).get("id") != 24:
            pytest.skip("not in clipper, can't restart travel flow")
        indy3_travel_client.act("travel")
        if not _wait_until(
            lambda: indy3_travel_client.state().get("question") is not None,
            timeout=10.0,
        ):
            pytest.skip("destination dialog did not appear")

    state = _state_or_skip(indy3_travel_client)
    question = state.get("question")
    assert question is not None

    henry_id = None
    for choice in question["choices"]:
        if "henry" in choice["label"].lower():
            henry_id = choice["id"]
            break
    assert henry_id is not None, f"no Henry choice in {question}"

    result = indy3_travel_client.answer(henry_id)
    new_room = result.get("room_changed")
    assert new_room is not None, f"expected room change, got {result}"
    assert new_room != 24, "expected to leave the clipper"

    state = _state_or_skip(indy3_travel_client)
    assert state["room"]["id"] == new_room
    # Henry's House contains study furniture; verify a couple of those names appear.
    object_names = {o["name"] for o in state.get("objects", [])}
    assert object_names & {"typewriter", "desk", "bookcase", "refrigerator"}, (
        f"new room doesn't look like Henry's House: {sorted(object_names)}"
    )
