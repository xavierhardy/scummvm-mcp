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


def _question_choice_id(question: dict, label: str) -> int:
    """Return the choice id whose label matches *label*, else the first choice."""
    choices = question.get("choices", [])
    for c in choices:
        if c.get("label") == label:
            return c["id"]
    return choices[0]["id"]


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
    for _ in range(10):
        state = client.state()
        if state.get("room", {}).get("id") == STREET_ROOM and state.get("inventory"):
            break
        sleep(SETTLE_SECS)

    state = client.state()
    assert state["room"]["id"] == STREET_ROOM, f"Expected street room 9, got {state['room']}"
    assert "max_the_object" in state.get("inventory", []), (
        f"Max must be in inventory for the two-target action, got: {state.get('inventory')}"
    )

    # Find actor_4 (the kitten / cat courier on the left).
    actor_4 = find_object_by_name(state, "actor_4")
    assert actor_4 is not None, "actor_4 (kitten) not found in objects"

    # Step 1: talk to the kitten to open its topic dialog.
    question = None
    for _ in range(10):
        result = client.act("talk_to", actor_4)
        question = result.get("question") or client.state().get("question")
        if question:
            break
        sleep(SETTLE_SECS)
    assert question is not None, "talking to the kitten should open its topic dialog"
    labels = [c.get("label") for c in question.get("choices", [])]
    assert "question" in labels, f"expected a 'question' topic, got: {labels}"

    # Step 2: ask the "question" topic — the kitten admits swallowing the orders.
    qid = _question_choice_id(question, "question")
    ask_result = client.answer(qid)
    ask_messages = ask_result.get("messages", [])
    _assert_no_garbage(ask_messages)
    ask_blob = " ".join(m.get("text", "") for m in ask_messages).lower()
    assert "commissioner" in ask_blob or "swallowed" in ask_blob, (
        f"expected the kitten to admit swallowing the Commissioner's orders, got: "
        f"{[m.get('text') for m in ask_messages]}"
    )

    # The dialog should have closed before the next action.
    for _ in range(10):
        if client.state().get("question") is None:
            break
        sleep(SETTLE_SECS)

    # Step 3: use Max on the kitten to retrieve the swallowed carnival tickets.
    result = client.act("use", "max", actor_4)

    messages = result.get("messages", [])
    _assert_no_garbage(messages)
    texts = [m.get("text", "") for m in messages]
    combined = " ".join(texts).lower()
    assert len(texts) > 0, "Expected dialog messages from using Max on the kitten, got none"
    # The interaction produces Sam/Max's reaction lines, e.g.
    # "Ooh, that gives me an idea!" / "...something bizarre is happening at the carnival."
    assert (
        "gives me an idea" in combined
        or "carnival" in combined
        or "inside-out" in combined
        or "inside out" in combined
        or "this guy" in combined
    ), f"Expected Sam & Max's reaction about the kitten, got: {texts}"

    # Verify the inventory now contains carnival_tickets. The inventory_added in
    # the result may lag, so also poll the live state.
    inventory_added = result.get("inventory_added", [])
    current_inventory = client.state().get("inventory", [])
    for _ in range(20):
        if "carnival_tickets" in inventory_added or "carnival_tickets" in current_inventory:
            break
        sleep(SETTLE_SECS)
        current_inventory = client.state().get("inventory", [])

    assert "carnival_tickets" in inventory_added or "carnival_tickets" in current_inventory, (
        f"carnival_tickets not acquired; inventory_added={inventory_added}, "
        f"current={current_inventory}"
    )

    # Verify the carnival_tickets object state changed from 0 to 1.
    objects_changed = result.get("objects_changed", [])
    carnival_obj_change = next(
        (oc for oc in objects_changed if "carnival" in oc.get("name", "").lower()),
        None,
    )
    if carnival_obj_change:
        assert carnival_obj_change["old_state"] == 0, (
            f"carnival_tickets old_state should be 0, got {carnival_obj_change}"
        )
        assert carnival_obj_change["new_state"] == 1, (
            f"carnival_tickets new_state should be 1, got {carnival_obj_change}"
        )
