"""
Integration tests for The Dig (DOS demo, SCUMM V7), wreck save slot 5.

Save slot 5 (dig-demo.s05) loads wreck room 19 with the takeable wire and
Brink present. Runs against its own fixture/instance so it can execute in
parallel with the canyon tests in test_dig.py (pytest-xdist --dist=loadgroup).
"""

from utils import McpClient


def test_11_dig_pickup_deposits_item(dig_wreck_client: McpClient) -> None:
    """Picking up a scene object must deposit it in the inventory, not leave it
    stuck on the cursor.

    Regression test: in The Dig, grabbing a takeable object latches it onto the
    mouse as the held item, so every following click became "use <item> on X"
    (the hero's "I can't use these things together" / "I don't think (s)he'd
    want that" refusals). Because each MCP action is self-contained, the bridge
    now simulates the player's right-click after a pickup to drop the item into
    the inventory and restore the default cursor.
    """
    client = dig_wreck_client
    state = client.state()
    assert state["room"]["id"] == 19, f"Expected wreck room 19, got {state['room']}"

    wire = next((o for o in state["objects"] if o["name"] == "wire"), None)
    assert wire is not None, f"No 'wire' object in scene: {[o['name'] for o in state['objects']]}"

    result = client.act("interact", wire["id"])
    inv = client.state().get("inventory", [])
    assert "wire" in inv, f"Wire was not deposited into inventory: {inv} ({result})"

    # Cursor must be free now: interacting with Brink should TALK (open the
    # conversation), not try to use the just-grabbed wire on him. If the wire
    # were still held, this would instead produce a use-on-actor refusal and no
    # dialog.
    talk = client.act("interact", "brink")
    talk_text = " ".join(m["text"].lower() for m in talk.get("messages", []))
    assert "these things together" not in talk_text, (
        f"Cursor still stuck holding the wire: {talk.get('messages')}"
    )
    assert "want that" not in talk_text, (
        f"Cursor still stuck holding the wire: {talk.get('messages')}"
    )
    assert client.state().get("question") is not None, (
        "Expected talking to Brink to open a dialog once the cursor was freed"
    )

    # Clean up: leave the conversation so the session fixture ends cleanly.
    for _ in range(10):
        q = client.state().get("question")
        if not q:
            break
        client.answer(len(q["choices"]))
