"""
Integration tests for the Indiana Jones 3 travel flow of Passport to Adventure.

Save slot 4 (pass.s04) drops Indy in the Pan Am clipper (room 24), where the
'Travel' verb on the verb bar opens the destination dialog ('To Henry's House'
/ 'Cancel'). Runs against its own fixture/instance so it can execute in
parallel with the boxing-gym tests in test_indy3.py (pytest-xdist
--dist=loadgroup).
"""

from __future__ import annotations

from time import sleep

import pytest
from utils import (
    McpClient,
    _state_or_skip,
    _wait_until,
    choice_labels,
    find_choice_id_containing,
    make_verbs,
    object_names,
    wait_until_or_skip,
)


def _open_destination_dialog(client: McpClient) -> dict | None:
    """Open the clipper's travel destination dialog and return its question."""
    question = client.state().get("question")
    if question is None:
        if (client.state().get("room") or {}).get("id") != 24:
            pytest.skip("not in clipper room")
        client.act("travel")
        _wait_until(lambda: client.state().get("question") is not None, timeout=10.0)
        question = client.state().get("question")
    return question


def _ensure_henrys_house(client: McpClient) -> dict:
    """Make sure Indy is in Henry's house, running the travel flow from the
    clipper if needed, and return the current state.

    Self-contained so the hidden-object test can run on its own fresh instance.
    Skips if Henry's house can't be reached.
    """
    state = _state_or_skip(client)
    room = (state.get("room") or {}).get("id")
    if room == 24:  # in the clipper — drive the travel flow to Henry's house
        question = _open_destination_dialog(client)
        if question is None:
            pytest.skip("destination dialog did not appear")
        henry_id = find_choice_id_containing(question, "henry")
        if henry_id is None:
            pytest.skip("no 'Henry' destination offered")
        client.answer(henry_id)
        _wait_until(
            lambda: client.state().get("room", {}).get("id") not in (None, 24),
            timeout=15.0,
        )
        state = _state_or_skip(client)
        room = (state.get("room") or {}).get("id")
    if room is None or room == 24:
        pytest.skip("could not reach Henry's house")
    return state


def _act_error_when_settled(
    client: McpClient, verb: str, target: str, attempts: int = 20
) -> str:
    """Retry act(verb, target) past 'not accepting input'; return the error text.

    Raises AssertionError if the action unexpectedly succeeds on a hidden target.
    """
    last = ""
    for _ in range(attempts):
        try:
            client.act(verb, target)
            raise AssertionError(f"acting on hidden {target!r} unexpectedly succeeded")
        except RuntimeError as e:
            last = str(e)
            if "not accepting input" in last:
                sleep(1.0)
                continue
            break
    return last


# ---------------------------------------------------------------------------
# Travel tests (save slot 4: Pan Am clipper)
# ---------------------------------------------------------------------------


def test_10_indy3_travel_initial_state(indy3_travel_client: McpClient) -> None:
    """Save slot 4 loads the airplane scene (room 24) with 'travel' on the bar."""
    wait_until_or_skip(
        lambda: indy3_travel_client.state().get("room") is not None,
        "save did not reach interactive state",
    )

    state = _state_or_skip(indy3_travel_client)
    room = state["room"]
    assert room["id"] == 24, f"expected room 24 (clipper), got {room}"
    verbs = set(state.get("verbs", []))
    sorted_verbs = sorted(verbs)
    assert "travel" in verbs, f"expected 'travel' verb, got: {sorted_verbs}"


def test_11_indy3_travel_opens_destination_dialog(
    indy3_travel_client: McpClient,
) -> None:
    """`act('travel')` opens the destination dialog (To Henry's House / Cancel)."""
    question = _open_destination_dialog(indy3_travel_client)
    assert question is not None, "expected the destination dialog"
    labels = choice_labels(question)
    henry_id = find_choice_id_containing(question, "henry")
    cancel_id = find_choice_id_containing(question, "cancel")
    assert henry_id is not None, f"expected a 'Henry' choice, got {labels}"
    assert cancel_id is not None, f"expected a 'Cancel' choice, got {labels}"


def test_12_indy3_travel_to_henrys_house(indy3_travel_client: McpClient) -> None:
    """Choosing 'To Henry's House' transports Indy to the new room."""
    question = _open_destination_dialog(indy3_travel_client)
    assert question is not None, "expected the travel destination dialog to be pending"
    henry_id = find_choice_id_containing(question, "henry")
    assert henry_id is not None, f"no Henry choice in {question}"

    result = indy3_travel_client.answer(henry_id)
    new_room = result.get("room_changed")
    assert new_room is not None, f"expected room change, got {result}"
    assert new_room != 24, "expected to leave the clipper"

    state = _state_or_skip(indy3_travel_client)
    room_id = state["room"]["id"]
    assert room_id == new_room, f"state room {room_id} != room_changed {new_room}"
    # Henry's House contains study furniture; verify a couple of those names appear.
    names = object_names(state)
    sorted_names = sorted(names)
    furniture = {"typewriter", "desk", "bookcase", "refrigerator"}
    assert names & furniture, f"not Henry's House: {sorted_names}"


def test_13_indy3_hidden_objects_not_selectable(
    indy3_travel_client: McpClient,
) -> None:
    """Objects concealed by a parent object are neither listed nor targetable.

    In Henry's house the sticky tape hides behind the bookcase, the chest
    under the table cloth, and the old book inside the (locked) chest. The
    engine's findObject() gates clicks on the parent object's state; the MCP
    bridge must mirror that. Self-contained: travels to Henry's house first.
    """
    state = _ensure_henrys_house(indy3_travel_client)
    pick_up, pull = make_verbs(indy3_travel_client, "pick up", "pull")

    names = object_names(state)
    hidden = {"old_book", "sticky_tape", "chest"}
    assert hidden.isdisjoint(
        names
    ), f"hidden objects leaked into state: {hidden & names}"

    # The room may still be settling after the travel cutscene; retry while
    # the engine reports input locked, then assert the hidden target fails.
    last = _act_error_when_settled(indy3_travel_client, "look", "old_book")
    assert "unknown target1" in last, f"unexpected error: {last}"

    # Reveal chain: move the plant off the cloth, pull the cloth — the chest
    # becomes selectable. The chest is locked, so the book inside stays hidden.
    pick_up("plant")
    sleep(1)
    pull("table_cloth")
    sleep(1)
    names = object_names(indy3_travel_client.state())
    sorted_names = sorted(names)
    assert "chest" in names, f"chest not revealed: {sorted_names}"
    assert "old_book" not in names, "book inside the locked chest must stay hidden"
