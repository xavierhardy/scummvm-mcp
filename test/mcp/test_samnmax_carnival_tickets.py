"""Test for Sam & Max: acquiring carnival_tickets from the cat courier.

This regression test validates the full street puzzle sequence:

1. Talk to the kitten (the cat courier, actor_4) to open its topic dialog.
2. Ask the "question" topic — the kitten admits it swallowed the
   Commissioner's orders.
3. Use Max on the kitten (a two-target "give" interaction dispatched via
   doSentence) to shake the orders back up, which adds 'carnival_tickets' to
   the inventory and flips the carnival_tickets object state from 0 to 1.

Save slot 2 (samnmax.s02) is the street scene (room 9) with Max already
exposed in the inventory as 'max_the_object', ready for two-target
interactions.
"""

from time import sleep

import pytest

from assertions import assert_no_talkie_garbage
from utils import (
    McpClient,
    bind_verb,
    choice_labels,
    find_choice_id,
    find_object_by_name,
    joined_message_text,
    message_texts,
)

SETTLE_SECS = 0.5
STREET_ROOM = 9


def _wait_street_with_inventory(client: McpClient, tries: int = 10) -> dict:
    """Poll until the street room is loaded with a populated inventory."""
    for _ in range(tries):
        state = client.state()
        if state.get("room", {}).get("id") == STREET_ROOM and state.get("inventory"):
            return state
        sleep(SETTLE_SECS)
    return client.state()


def _open_kitten_dialog(client: McpClient, actor, tries: int = 10) -> dict | None:
    """Talk to the kitten until its topic dialog opens; return the question."""
    for _ in range(tries):
        result = client.act("talk_to", actor)
        question = result.get("question") or client.state().get("question")
        if question:
            return question
        sleep(SETTLE_SECS)
    return None


def _wait_dialog_closed(client: McpClient, tries: int = 10) -> None:
    """Poll until no dialog question remains pending."""
    for _ in range(tries):
        if client.state().get("question") is None:
            return
        sleep(SETTLE_SECS)


def _wait_inventory_has(client: McpClient, item: str, tries: int = 20) -> list:
    """Poll the live inventory until it contains *item*; return the inventory."""
    inventory = client.state().get("inventory", [])
    for _ in range(tries):
        if item in inventory:
            return inventory
        sleep(SETTLE_SECS)
        inventory = client.state().get("inventory", [])
    return inventory


def _admits_swallowing(result: dict) -> bool:
    """True if the kitten admits swallowing the Commissioner's orders."""
    blob = joined_message_text(result).lower()
    return "commissioner" in blob or "swallowed" in blob


def _mentions_reaction(result: dict) -> bool:
    """True if Sam/Max react to shaking the kitten (any of several lines)."""
    blob = joined_message_text(result).lower()
    phrases = ("gives me an idea", "carnival", "inside-out", "inside out", "this guy")
    return any(p in blob for p in phrases)


def _carnival_change(objects_changed: list) -> dict | None:
    """Return the carnival_tickets object-change record, if present."""
    for oc in objects_changed:
        if "carnival" in oc.get("name", "").lower():
            return oc
    return None


def _assert_carnival_state_flip(change: dict | None) -> None:
    """If the carnival_tickets object change is present, it must flip 0 -> 1."""
    if change is None:
        return
    assert change["old_state"] == 0, f"carnival_tickets old_state should be 0: {change}"
    assert change["new_state"] == 1, f"carnival_tickets new_state should be 1: {change}"


@pytest.mark.slow
def test_samnmax_s02_cat_courier_gives_carnival_tickets(
    samnmax_street_client: McpClient,
) -> None:
    """The full cat-courier sequence must end with carnival_tickets in hand.

    Talking to the kitten opens its dialog; asking the "question" topic makes it
    admit it swallowed the Commissioner's orders; then using Max on the kitten
    retrieves them as the carnival_tickets inventory item.
    """
    client = samnmax_street_client

    # Verify we're in the street room with Max available in inventory.
    state = _wait_street_with_inventory(client)
    room = state["room"]
    inventory = state.get("inventory", [])
    assert room["id"] == STREET_ROOM, f"Expected street room 9, got {room}"
    assert "max_the_object" in inventory, f"Max must be in inventory, got: {inventory}"

    # Find actor_4 (the kitten / cat courier on the left).
    actor_4 = find_object_by_name(state, "actor_4")
    assert actor_4 is not None, "actor_4 (kitten) not found in objects"

    # Step 1: talk to the kitten to open its topic dialog.
    question = _open_kitten_dialog(client, actor_4)
    assert question is not None, "talking to the kitten should open its topic dialog"
    labels = choice_labels(question)
    assert "question" in labels, f"expected a 'question' topic, got: {labels}"

    # Step 2: ask the "question" topic — the kitten admits swallowing the orders.
    qid = find_choice_id(question, "question")
    ask_result = client.answer(qid)
    assert_no_talkie_garbage(ask_result.get("messages", []))
    ask_texts = message_texts(ask_result)
    assert _admits_swallowing(ask_result), f"no admission: {ask_texts}"

    # The dialog should have closed before the next action.
    _wait_dialog_closed(client)

    # Step 3: use Max on the kitten to retrieve the swallowed carnival tickets.
    use = bind_verb(client, "use")
    result = use("max", actor_4)
    messages = result.get("messages", [])
    assert_no_talkie_garbage(messages)
    texts = message_texts(result)
    assert texts, "Expected dialog messages from using Max on the kitten, got none"
    # The interaction produces Sam/Max's reaction lines, e.g.
    # "Ooh, that gives me an idea!" / "...bizarre is happening at the carnival."
    assert _mentions_reaction(result), f"Expected Sam & Max's reaction, got: {texts}"

    # Verify the inventory now contains carnival_tickets. The inventory_added in
    # the result may lag, so also poll the live state.
    inventory_added = result.get("inventory_added", [])
    current_inventory = _wait_inventory_has(client, "carnival_tickets")
    acquired = (
        "carnival_tickets" in inventory_added or "carnival_tickets" in current_inventory
    )
    assert acquired, f"no tickets; added={inventory_added} cur={current_inventory}"

    # Verify the carnival_tickets object state changed from 0 to 1.
    objects_changed = result.get("objects_changed", [])
    _assert_carnival_state_flip(_carnival_change(objects_changed))
