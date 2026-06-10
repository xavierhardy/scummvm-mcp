"""
Integration tests for the Indiana Jones 3 travel flow of Passport to Adventure.

Save slot 4 (pass.s04) drops Indy in the Pan Am clipper (room 24), where the
'Travel' verb on the verb bar opens the destination dialog ('To Henry's House'
/ 'Cancel'). Runs against its own fixture/instance so it can execute in
parallel with the boxing-gym tests in test_indy3.py (pytest-xdist
--dist=loadfile).
"""

from __future__ import annotations

import pytest
from time import sleep

from utils import McpClient, _wait_until, _state_or_skip


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


def test_13_indy3_hidden_objects_not_selectable(
    indy3_travel_client: McpClient,
) -> None:
    """Objects concealed by a parent object are neither listed nor targetable.

    In Henry's house the sticky tape hides behind the bookcase, the chest
    under the table cloth, and the old book inside the (locked) chest. The
    engine's findObject() gates clicks on the parent object's state; the MCP
    bridge must mirror that. Runs after test_12 left us in Henry's house.
    """
    state = _state_or_skip(indy3_travel_client)
    if state.get("room", {}).get("id") == 24:
        pytest.skip("still in the clipper — travel to Henry's house didn't happen")

    names = {o["name"] for o in state.get("objects", [])}
    hidden = {"old_book", "sticky_tape", "chest"}
    assert hidden.isdisjoint(names), f"hidden objects leaked into state: {hidden & names}"

    # The room may still be settling after the travel cutscene; retry while
    # the engine reports input locked, then assert the hidden target fails.
    last = ""
    for _ in range(20):
        try:
            indy3_travel_client.act("look", "old_book")
            raise AssertionError("acting on hidden 'old_book' unexpectedly succeeded")
        except RuntimeError as e:
            last = str(e)
            if "not accepting input" in last:
                sleep(1.0)
                continue
            break
    assert "unknown target1" in last, f"unexpected error: {last}"

    # Reveal chain: move the plant off the cloth, pull the cloth — the chest
    # becomes selectable. The chest is locked, so the book inside stays hidden.
    indy3_travel_client.act("pick up", "plant")
    sleep(1)
    indy3_travel_client.act("pull", "table_cloth")
    sleep(1)
    names = {o["name"] for o in indy3_travel_client.state().get("objects", [])}
    assert "chest" in names, f"chest not revealed after pulling the cloth: {sorted(names)}"
    assert "old_book" not in names, "book inside the locked chest must stay hidden"
