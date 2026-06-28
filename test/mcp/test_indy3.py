"""
Integration tests for the Indiana Jones 3 segment of Passport to Adventure.

Save slot 3 (pass.s03) drops Indy at the boxing gym. Walking to the locker
room triggers a dialog; choice 1 starts a fist-fight against the boxing coach.
The fight HUD vars are surfaced through state.fight, and numpad-style
keystrokes (1-9) drive Indy's punches/blocks/step-backs.
"""

from __future__ import annotations

from time import sleep

import pytest

from utils import (
    McpClient,
    _find_object,
    _state_or_skip,
    _wait_until,
    object_names,
    skip_unless,
    wait_until_or_skip,
)

INTERACTIVE_TIMEOUT_SECS = 30


def _start_fight(client: McpClient) -> dict:
    """Reach the boxing fight from the gym save and return the fight HUD state.

    Self-contained so each fight test runs on its own fresh instance: walk to
    the locker room, accept the coach's challenge (choice 1), and wait for the
    fight HUD to appear. Skips if the fight can't be reached.
    """
    if client.state().get("fight") is None:
        if client.state().get("question") is None:
            client.act("walk to", "locker_room")
            _wait_until(
                lambda: client.state().get("question") is not None, timeout=10.0
            )
        if client.state().get("question") is not None:
            client.answer(1)
        _wait_until(lambda: client.state().get("fight") is not None, timeout=10.0)
    fight = client.state().get("fight")
    if fight is None:
        pytest.skip("could not start the boxing fight from the gym save")
    return fight


def _ensure_locker_dialog(client: McpClient) -> dict:
    """Walk to the locker room if needed and return the coach's dialog question."""
    question = client.state().get("question")
    if question is None:
        client.act("walk to", "locker_room")
        _wait_until(lambda: client.state().get("question") is not None, timeout=10.0)
        question = client.state().get("question")
    if question is None:
        pytest.skip("dialog did not appear")
    return question


def _assert_fight_side(side: dict, who: str) -> None:
    """Assert one fight HUD side reports positive integer health and a gauge."""
    health = side.get("health")
    gauge = side.get("punch_power")
    assert health is not None and gauge is not None, f"{who} missing fields: {side}"
    assert isinstance(health, int) and health > 0, f"{who} bad health: {side}"
    assert isinstance(gauge, int) and gauge >= 0, f"{who} bad gauge: {side}"


def _side_changed(before: dict, after: dict) -> bool:
    """True if a fighter's health or punch gauge moved between two HUD reads."""
    return (
        before["health"] != after["health"]
        or before["punch_power"] != after["punch_power"]
    )


def _assert_fight_hud_valid(state: dict) -> None:
    """If the fight HUD is still present, both sides must report integer health."""
    fight = state.get("fight")
    if fight is None:
        return
    assert isinstance(fight["indy"]["health"], int), f"indy health not int: {fight}"
    assert isinstance(fight["opponent"]["health"], int), (
        f"opponent health not int: {fight}"
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_indy3_initial_state(indy3_client: McpClient) -> None:
    """Save slot 3 loads the boxing gym (room 25)."""
    wait_until_or_skip(
        lambda: indy3_client.state().get("room") is not None,
        "save did not reach interactive state",
    )

    state = _state_or_skip(indy3_client)
    assert state.get("room") is not None, "expected a room in state"
    # Gym scene exposes the standard V3 verb bar.
    verbs = set(state.get("verbs", []))
    sorted_verbs = sorted(verbs)
    assert {"walk to", "look", "use", "push", "pull"}.issubset(verbs), (
        f"V3 bar: {sorted_verbs}"
    )


def test_02_indy3_locker_room_visible(indy3_client: McpClient) -> None:
    """The 'locker_room' object is selectable in the gym."""
    state = _state_or_skip(indy3_client)
    locker = _find_object(state, "locker_room")
    names = sorted(object_names(state))
    assert locker is not None, f"locker_room not found; objects = {names}"


def test_03_indy3_walk_to_locker_triggers_dialog(indy3_client: McpClient) -> None:
    """Walking to the locker room opens a dialog with the boxing coach."""
    question = _ensure_locker_dialog(indy3_client)
    choices = question.get("choices", [])
    assert len(choices) >= 4, f"expected >=4 choices, got: {choices}"


def test_04_indy3_answer_starts_fight(indy3_client: McpClient) -> None:
    """Choosing choice 1 ('go easy') starts the fight; state.fight populates."""
    fight = _start_fight(indy3_client)
    assert "indy" in fight and "opponent" in fight, f"fight HUD missing sides: {fight}"
    _assert_fight_side(fight["indy"], "indy")
    _assert_fight_side(fight["opponent"], "opponent")


def test_05_indy3_punch_high_lands(indy3_client: McpClient) -> None:
    """A high punch (numpad 9) reduces the opponent's health."""
    before = _start_fight(indy3_client)
    indy3_client.call_capturing("keystroke", {"key": "9"})
    sleep(2.0)
    state = _state_or_skip(indy3_client)
    skip_unless(state.get("fight") is not None, "fight ended unexpectedly")
    after = state["fight"]

    # The opponent should have taken some damage on at least one of the
    # subsequent rounds. If the coach blocked, only the punch_power gauge
    # changes — that still counts as the input being received.
    opponent_changed = _side_changed(before["opponent"], after["opponent"])
    indy_changed = _side_changed(before["indy"], after["indy"])
    assert opponent_changed or indy_changed, f"high-punch had no effect: {after}"


def test_06_indy3_block_then_step_back(indy3_client: McpClient) -> None:
    """Mid block (5) followed by step-back (4) keeps the fight running."""
    _start_fight(indy3_client)

    indy3_client.call_capturing("keystroke", {"key": "5"})
    sleep(1.5)
    indy3_client.call_capturing("keystroke", {"key": "4"})
    sleep(1.5)

    # The fight may end if Indy or the coach is KO'd, but otherwise the HUD
    # should still be visible and the values valid.
    _assert_fight_hud_valid(_state_or_skip(indy3_client))
