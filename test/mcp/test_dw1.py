"""
Integration test for the Discworld CD demo (Tinsel engine, V1).

Discworld has no verb bar: the whole vocabulary is which button was pressed
over what — a click walks there, a double click is the action, a right click
looks. The bridge exposes exactly those three, and gets them to land the way a
player does: it parks the cursor on the target and lets the engine's own tag
process notice before raising the event, because what the game acts on is what
it thinks is being pointed at, not a coordinate.

The other thing worth watching here is the names. Scenery carries its label in
the scene data, so the snapshot reads it straight out ("wardrobe", "luggage").
An inventory item carries none at all — the game only ever says what an item is
by running that item's own script — so the bridge asks each item's script for
its name while the game idles. A pouch that comes back as "pouch" rather than
as a number is that machinery working.

The demo has no save to load: the whole run is one ordered sequence on a single
fresh instance (like woodruff and the atlantis/ft demos), skipping the intro
first.
"""

import base64

import pytest

from mcp_client import McpClient

# One ordered walkthrough on a single instance — pin it to one xdist worker.
pytestmark = [pytest.mark.xdist_group("dw1")]

# Where the demo drops the player once the intro is over.
BEDROOM = "rinceroo"


def _skip_intro(client: McpClient, max_skips: int = 15) -> dict:
    """Escape through the intro until a playable scene is up; return its state."""
    for _ in range(max_skips):
        state = client.state()
        if state.get("can_act") and state.get("objects"):
            return state
        try:
            client.skip()
        except RuntimeError as exc:  # a skip with nothing to cut short
            if "starting up" not in str(exc):
                raise
    raise AssertionError("the intro never gave way to a playable scene")


@pytest.fixture(scope="session")
def playing(dw1_client: McpClient) -> McpClient:
    """The demo, past the intro and accepting input."""
    _skip_intro(dw1_client)
    return dw1_client


def _names(state: dict) -> list[str]:
    return [obj["name"] for obj in state["objects"]]


def test_01_dw1_state_describes_the_scene(playing: McpClient) -> None:
    """The snapshot is the scene as the game labels it, not as pixels."""
    state = playing.state()

    assert state["room"]["name"] == BEDROOM, f"unexpected scene: {state['room']}"
    assert state["can_act"] is True, f"the game is not accepting input: {state}"
    assert state["verbs"] == ["walk_to", "look_at", "use"], f"unexpected verbs: {state}"

    # The scenery is named by the game's own tag strings.
    names = _names(state)
    for expected in ("door", "wardrobe", "bed", "luggage", "diploma"):
        assert expected in names, f"{expected} missing from {names}"

    # Every target carries a spot to point at, inside the picture.
    for obj in state["objects"]:
        assert obj["kind"] in ("character", "scenery"), obj
        assert 0 <= obj["x"] < 320 and 0 <= obj["y"] < 200, f"off screen: {obj}"

    # The door leads out of the room, and says so.
    door = next(obj for obj in state["objects"] if obj["name"] == "door")
    assert door.get("pathway") is True, f"the door is not marked as a way out: {door}"

    # Nothing carried yet, and nobody is asking anything.
    assert state["inventory"] == [], f"unexpected inventory: {state}"
    assert "held_item" not in state, f"something is already in hand: {state}"
    assert "question" not in state, f"unexpected question: {state}"

    # The player character has a position of his own, and is not one of the
    # things he can be told to act on.
    assert state["position"]["x"] > 0 and state["position"]["y"] > 0, state


def test_02_dw1_look_at_reports_what_is_said(playing: McpClient) -> None:
    """`look_at` is the right click: the character examines the thing aloud."""
    result = playing.act("look_at", "diploma")

    lines = result.get("messages", [])
    assert lines, f"looking at the diploma said nothing: {result}"
    assert all(line["type"] == "actor" for line in lines), lines
    assert all(line.get("actor") for line in lines), (
        f"lines came back with no speaker: {lines}"
    )
    # The demo's diploma speech is about failing Thurmaturgy 101.
    said = " ".join(line["text"] for line in lines).lower()
    assert "unseen university" in said, f"that is not the diploma speech: {said}"

    # Reading state clears the queue, so the same lines are not replayed.
    assert playing.state()["messages"] == [], "the lines were reported twice"


def test_03_dw1_use_changes_the_scene(playing: McpClient) -> None:
    """`use` is the double click, and the scene reports what it revealed."""
    before = _names(playing.state())
    assert "pouch" not in before, f"the wardrobe is already open: {before}"

    result = playing.act("use", "wardrobe")

    changed = result.get("objects_changed", [])
    assert any(c["name"] == "pouch" and c["new_state"] == "present" for c in changed), (
        f"opening the wardrobe revealed nothing: {result}"
    )
    assert "pouch" in _names(playing.state())


def test_04_dw1_taking_an_item_names_it(playing: McpClient) -> None:
    """An item enters the inventory under the name the game itself prints.

    Nothing in the data maps an item to a name — only the item's own script
    does, when the player points at it. The bridge asks every item's script for
    its name while the game idles, which is why this reads "pouch" and not a
    number."""
    result = playing.act("use", "pouch")

    assert result.get("inventory_added") == ["pouch"], (
        f"the pouch was not picked up under its own name: {result}"
    )
    assert result.get("held_item") == "pouch", f"it is not in hand: {result}"

    state = playing.state()
    assert [(i["name"], i.get("held")) for i in state["inventory"]] == [("pouch", True)]
    assert state["held_item"] == "pouch"
    # And it is no longer part of the scene.
    assert "pouch" not in _names(state)


def test_05_dw1_use_with_an_item_in_hand(playing: McpClient) -> None:
    """`target2` is the item to do it with — that is 'use X on Y' here."""
    result = playing.act("use", "luggage", "pouch")

    assert playing.state()["held_item"] == "pouch", f"the hand was emptied: {result}"
    # The luggage will not budge, and Rincewind says so rather than nothing.
    said = " ".join(line["text"] for line in result.get("messages", [])).lower()
    assert said, f"using the pouch on the luggage said nothing: {result}"


def test_06_dw1_walk_moves_the_character(playing: McpClient) -> None:
    """walk() takes a point and the call waits for the walk to finish."""
    before = playing.state()["position"]

    result = playing.walk(230, 180)
    assert "position" in result, f"walk reported no position: {result}"

    after = playing.state()["position"]
    assert after != before, f"he never moved (still at {after})"
    assert result["position"] == after, (
        f"walk returned {result['position']} but he stands at {after}"
    )

    # Out-of-bounds coordinates are clamped rather than refused.
    playing.walk(9999, 9999)
    assert playing.state()["can_act"] is True


def test_07_dw1_act_rejects_what_it_cannot_do(playing: McpClient) -> None:
    """The errors say what to do instead, rather than failing silently."""
    with pytest.raises(RuntimeError, match="unknown verb"):
        playing.act("juggle", "bed")
    with pytest.raises(RuntimeError, match="unknown target"):
        playing.act("look_at", "hippopotamus")
    with pytest.raises(RuntimeError, match="must be an item you are carrying"):
        playing.act("use", "bed", "hippopotamus")

    # Nothing is asking a question, so there is nothing to answer.
    with pytest.raises(RuntimeError, match="no conversation question is pending"):
        playing.call_tool("answer", {"id": 1})


def test_08_dw1_targets_resolve_by_id_too(playing: McpClient) -> None:
    """Scenery can be named or given by the id the snapshot published."""
    bed = next(obj for obj in playing.state()["objects"] if obj["name"] == "bed")

    result = playing.act("look_at", bed["id"])
    said = " ".join(line["text"] for line in result.get("messages", [])).lower()
    assert "bed" in said, f"that is not the bed speech: {result}"


def test_09_dw1_debug_reads_the_engine(playing: McpClient) -> None:
    """The debug tool reports what the engine thinks, for diagnosis."""
    system = playing.call_tool("debug")["system"]

    assert system["scene_name"] == BEDROOM, system
    assert system["version"] == 1, f"not the version this demo runs on: {system}"
    assert system["control"] is True and system["can_act"] is True, system
    assert system["held_item"] > 0, f"the pouch should still be in hand: {system}"

    items = playing.call_tool("debug", {"items": True, "system": False})["items"]
    assert [item["name"] for item in items] == ["pouch"], items

    objects = playing.call_tool("debug", {"objects": True, "system": False})["objects"]
    assert any(obj["name"] == "wardrobe" and not obj["actor"] for obj in objects)


def test_10_dw1_raw_input_tools_are_accepted(playing: McpClient) -> None:
    """The raw input tools work here too, for an agent driving by screenshot."""
    playing.call_tool("mouse_move", {"x": 160, "y": 100})
    playing.call_tool("keystroke", {"key": "Escape"})

    result = playing.call_tool_raw("screenshot")
    structured = result["structuredContent"]
    assert structured["width"] == 320 and structured["height"] == 200, structured

    images = [b for b in result["content"] if b.get("type") == "image"]
    assert len(images) == 1, f"no image in the result: {result['content']}"
    assert base64.b64decode(images[0]["data"])[:4] == b"\x89PNG"
