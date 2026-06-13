"""
Test for Sam & Max: acquiring carnival_tickets via use Max on actor_4 (the kitten).

This regression test validates the two-target natural interface action in Sam & Max,
where using Max on the street kitten (actor_4) triggers a specific dialog sequence
and adds carnival_tickets to the inventory.

Save slot 2 (samnmax.s02) is the street scene with Max already exposed in inventory
as 'max_the_object', ready for two-target interactions.
"""

import pytest
from time import sleep

from utils import McpClient, find_object_by_name


SETTLE_SECS = 0.5
STREET_ROOM = 9


def _assert_no_garbage(messages: list) -> None:
    """Every emitted message must be readable text, not a sound-code fragment.

    The talkie prefix decodes (via the game code page) to a non-breaking space
    (U+00A0) plus stray bytes. A clean line always carries real letters.
    """
    for m in messages:
        t = m.get("text", "")
        assert "\u00a0" not in t, f"talkie-code garbage leaked into a message: {t!r}"
        alpha_count = sum(1 for c in t if c.isascii() and c.isalpha())
        assert alpha_count >= 1, f"message has no readable letters: {t!r}"


def test_samnmax_s02_use_max_on_kitten_acquires_carnival_tickets(samnmax_street_client: McpClient) -> None:
    """
    Using Max on the street kitten (actor_4) must:
    1. Trigger dialog between Sam and Max about the kitten
    2. Add 'carnival_tickets' object to inventory
    3. Change the carnival_tickets object state from 0 to 1
    
    The dialog should reference the kitten (or "this guy") and inspire ideas.
    A new inventory item is added (Adding object 95/carnival_tickets from room 9 into inventory).
    """
    # Verify we're in the street room with Max available in inventory
    for _ in range(10):
        state = samnmax_street_client.state()
        if state.get("room", {}).get("id") == STREET_ROOM and state.get("inventory"):
            break
        sleep(SETTLE_SECS)
    
    state = samnmax_street_client.state()
    assert state["room"]["id"] == STREET_ROOM, f"Expected street room 9, got {state['room']}"
    assert "max_the_object" in state.get("inventory", []), (
        f"Max must be in inventory for two-target action, got: {state.get('inventory')}"
    )
    
    # Find actor_4 (the kitten on the left)
    actor_4 = find_object_by_name(state, "actor_4")
    assert actor_4 is not None, f"actor_4 (kitten) not found in objects"
    
    # Execute: use max on actor_4
    # This should convert to verb 3 (give) internally and trigger the correct dialog
    result = samnmax_street_client.act("use", "max", actor_4)
    
    # Verify the messages from the interaction
    messages = result.get("messages", [])
    _assert_no_garbage(messages)
    texts = [m.get("text", "") for m in messages]
    combined = " ".join(texts).lower()
    
    # Check for key dialog lines that indicate the correct interaction happened
    # The interaction produces lines like "I'd just love to turn this guy inside-out!"
    # and "Ooh, that gives me an idea!"
    assert len(texts) > 0, f"Expected dialog messages, got none"
    assert "turn this guy inside-out" in combined or "inside out" in combined or "this guy" in combined, (
        f"Expected Sam's reaction about the kitten, got: {texts}"
    )
    
    # Verify the inventory now contains carnival_tickets
    # The inventory_added in the result may be empty due to timing,
    # but the item is definitely added to the game state. Wait for it to settle.
    inventory_added = result.get("inventory_added", [])
    state_after = samnmax_street_client.state()
    current_inventory = state_after.get("inventory", [])
    
    # Wait for the inventory to settle if not immediately present
    for _ in range(20):
        if "carnival_tickets" in inventory_added or "carnival_tickets" in current_inventory:
            break
        sleep(SETTLE_SECS)
        current_inventory = samnmax_street_client.state().get("inventory", [])
    
    # Either the result reports it or the state reflects it
    assert "carnival_tickets" in inventory_added or "carnival_tickets" in current_inventory, (
        f"carnival_tickets not acquired; inventory_added={inventory_added}, current={current_inventory}"
    )
    
    # Verify the object state changed (from 0 to 1)
    objects_changed = result.get("objects_changed", [])
    carnival_obj_change = next(
        (oc for oc in objects_changed if "carnival" in oc.get("name", "").lower()),
        None
    )
    if carnival_obj_change:
        assert carnival_obj_change["old_state"] == 0, (
            f"carnival_tickets old_state should be 0, got {carnival_obj_change}"
        )
        assert carnival_obj_change["new_state"] == 1, (
            f"carnival_tickets new_state should be 1, got {carnival_obj_change}"
        )
