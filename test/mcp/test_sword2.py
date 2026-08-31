"""
Integration test for the Broken Sword II demo (sword2 engine).

The two Broken Sword games play the same way — one click on a thing runs its
interaction, a right click looks at it, no verb bar — so this offers the same
six verbs as `test_sword1.py` and means the same by each. What differs is what
the engine will tell you about the world.

The first game attaches no name to anything on screen, so its bridge carries
authored tables. This one names things itself: every mouse-detection box may
carry a text line, which the game paints next to the cursor when its "object
labels" option is on. The bridge reads that line out of the box whatever the
option says, so what the snapshot calls a thing is what the game would call it
— "Fence", "Steps", "Barge" — and the option only decides whether a *player*
sees it too. That is what test_02 is really checking.

The demo has no save to load: the whole run is one ordered sequence on a single
fresh instance, past its opening.
"""

import base64
import time

import pytest

from mcp_client import McpClient

# One ordered walkthrough on a single instance — pin it to one xdist worker.
pytestmark = [pytest.mark.xdist_group("sword2")]

VERBS = ["look_at", "interact", "use", "talk_to", "pick_up", "walk_to"]
# Where the demo hands over: the docks, outside the watchman's hut.
DOCKS = "screen_11"


def _wait_ready(client: McpClient, tries: int = 30) -> dict:
    """The state once the opening is over and the game accepts input."""
    for _ in range(tries):
        state = client.state()
        if state.get("can_act") and state.get("objects"):
            return state
        try:
            client.skip()
        except RuntimeError as exc:  # nothing to cut short yet
            if "starting up" not in str(exc):
                raise
    raise AssertionError("the opening never gave way to a playable screen")


@pytest.fixture(scope="session")
def playing(sword2_client: McpClient) -> McpClient:
    """The demo, past the opening and accepting input."""
    _wait_ready(sword2_client)
    return sword2_client


def _names(state: dict) -> list[str]:
    return [obj["name"] for obj in state["objects"]]


def _wait_inventory(client: McpClient, tries: int = 40) -> set[str]:
    """The carried objects, once the demo has handed them over."""
    carried: set[str] = set()
    for _ in range(tries):
        carried = {item["name"] for item in client.state()["inventory"]}
        if carried:
            return carried
        time.sleep(0.25)
    return carried


def _by_kind(state: dict, kind: str) -> list[dict]:
    return [obj for obj in state["objects"] if obj["kind"] == kind]


def test_01_sword2_state_describes_the_screen(playing: McpClient) -> None:
    """The snapshot is the screen as the game itself labels it."""
    state = playing.state()

    assert state["room"]["name"] == DOCKS, f"unexpected screen: {state['room']}"
    assert state["can_act"] is True, f"the game is not accepting input: {state}"
    assert state["verbs"] == VERBS, f"unexpected verbs: {state}"

    names = _names(state)
    for expected in ("hut", "barge", "debris", "sign", "steps"):
        assert expected in names, f"{expected} missing from {names}"

    # Names are unique, so one can be echoed straight back into act().
    assert len(names) == len(set(names)), f"two targets share a name: {names}"

    for obj in state["objects"]:
        assert obj["kind"] in ("object", "person", "item", "exit", "floor", "scroll"), (
            obj
        )
        x1, y1, x2, y2 = obj["box"]
        assert x2 >= x1 and y2 >= y1, f"empty box: {obj}"
        assert obj["position"]["x"] == (x1 + x2) // 2, obj

    # The steps lead off the screen, and say so.
    steps = next(obj for obj in state["objects"] if obj["name"] == "steps")
    assert steps["kind"] == "exit" and steps.get("pathway") is True, steps

    # The demo hands the player its three objects as the opening finishes, so
    # this waits for them rather than assuming they are there on the first
    # snapshot after input is handed over.
    carried = _wait_inventory(playing)
    assert {"dart", "grub", "lipstick"} <= carried, f"unexpected inventory: {carried}"
    assert "held_item" not in state, f"something is already in hand: {state}"
    assert state["position"]["x"] > 0 and state["position"]["y"] > 0, state


def test_02_sword2_names_come_from_the_games_own_labels(playing: McpClient) -> None:
    """A thing is called what the game would paint next to the cursor.

    The label lives on the mouse box, so the bridge has it without hovering and
    whatever the object-labels option says; the option only decides whether a
    player sees it. `debug` reports both candidate names, which is what makes
    the difference visible: the label is the readable one, the resource name is
    the one the game's authors used."""
    objects = playing.call_tool("debug", {"objects": True, "system": False})["objects"]

    labelled = [obj for obj in objects if obj["label"]]
    assert labelled, f"no object carried a label: {objects}"

    hut = next(obj for obj in objects if obj["name"] == "hut")
    assert hut["label"] == "Hut", f"the label is not the game's: {hut}"
    assert hut["resource_name"] == "hut_11", f"unexpected resource name: {hut}"
    # The published name is the label, folded into an identifier.
    assert hut["name"] == "hut"

    # Something with no label of its own still gets a usable name.
    unlabelled = [obj for obj in objects if not obj["label"]]
    assert unlabelled, "every object had a label, so the fallback is untested"
    assert all(obj["name"] for obj in unlabelled), unlabelled


def test_03_sword2_look_at_reports_what_is_said(playing: McpClient) -> None:
    """`look_at` is the right click: George examines the thing aloud."""
    result = playing.act("look_at", "sign")

    lines = result.get("messages", [])
    assert lines, f"looking at the sign said nothing: {result}"
    assert all(line["type"] == "actor" for line in lines), lines
    assert all(line.get("actor") == "george" for line in lines), (
        f"lines came back from the wrong speaker: {lines}"
    )

    # Reading state clears the queue, so the same lines are not replayed.
    assert playing.state()["messages"] == [], "the lines were reported twice"


def test_04_sword2_walk_moves_george(playing: McpClient) -> None:
    """walk() takes a point on one of the walkable areas — and arrives at it.

    The point aimed at is on the band George is already standing in, since
    that is walkable by definition; he stops on the nearest spot his own
    route-finder will put him, which is a few pixels off rather than exact."""
    before = playing.state()["position"]
    floor = _by_kind(playing.state(), "floor")
    assert floor, "the screen has no walkable area at all"

    target_x, target_y = before["x"] - 200, before["y"]
    result = playing.walk(target_x, target_y)
    assert "position" in result, f"walk reported no position: {result}"
    after = playing.state()["position"]
    assert after != before, f"he never moved (still at {after})"
    assert result["position"] == after, (
        f"walk returned {result['position']} but he stands at {after}"
    )
    assert abs(after["x"] - target_x) <= 60 and abs(after["y"] - target_y) <= 60, (
        f"asked for ({target_x}, {target_y}) but he stopped at {after}"
    )

    # A point off every walkable area is refused, and the error says where they are.
    with pytest.raises(RuntimeError, match="not on a walkable area"):
        playing.walk(-50, -50)


def test_05_sword2_walking_onto_something_is_refused(playing: McpClient) -> None:
    """A walkable area runs on underneath the things standing on it.

    The game gives a click to whatever is on top, so walking to a point that
    something covers is not walking at all — it is acting on that thing. The
    click the bridge would have made there is one the game quietly ignores, so
    say what is in the way instead of standing still without a word."""
    covered = [obj for obj in _by_kind(playing.state(), "object") if "box" in obj]
    assert covered, "nothing on this screen stands on the floor"
    target = covered[0]
    x, y = target["position"]["x"], target["position"]["y"]

    with pytest.raises(RuntimeError, match=f"covered by '{target['name']}'"):
        playing.walk(x, y)


def test_06_sword2_an_exit_takes_two_goes(playing: McpClient) -> None:
    """A way out is walked to first and taken second — as it is for a player."""
    start = playing.state()["room"]["id"]

    playing.act("walk_to", "steps")
    if playing.state()["room"]["id"] == start:
        # He had to walk over first; the second go leaves.
        result = playing.act("walk_to", "steps")
        assert result.get("room_changed"), f"the second go did not leave: {result}"

    state = playing.state()
    assert state["room"]["id"] != start, f"still on the same screen: {state['room']}"
    assert state["can_act"] is True, f"input was not handed back: {state}"
    assert state["objects"], "the new screen reported nothing to point at"


def test_07_sword2_picking_something_up(playing: McpClient) -> None:
    """An item joins the inventory under the name the game gives it."""
    names = _names(playing.state())
    assert "hook" in names, f"the boat hook is not on this screen: {names}"

    result = playing.act("pick_up", "hook")
    assert result.get("inventory_added") == ["boathook"], (
        f"the hook was not picked up: {result}"
    )
    assert "boathook" in [i["name"] for i in playing.state()["inventory"]]


def test_08_sword2_using_an_item_on_something(playing: McpClient) -> None:
    """`target2` is the item to do it with, and the game answers per item."""
    dog = _by_kind(playing.state(), "person")
    assert dog, "the dog is not on this screen yet"

    with_dart = playing.act("use", dog[0]["name"], "dart")
    with_grub = playing.act("use", dog[0]["name"], "grub")

    said_dart = " ".join(m["text"] for m in with_dart.get("messages", []))
    said_grub = " ".join(m["text"] for m in with_grub.get("messages", []))
    assert said_dart and said_grub, (
        f"neither item drew a response: {with_dart} / {with_grub}"
    )
    assert said_dart != said_grub, (
        "both items drew the same line, so the held item never reached the game"
    )

    # And the hand is left empty afterwards, as it is when the game takes over.
    assert "held_item" not in playing.state(), "an item was left in hand"


def test_09_sword2_act_rejects_what_it_cannot_do(playing: McpClient) -> None:
    """The errors say what to do instead, rather than failing silently."""
    with pytest.raises(RuntimeError, match="unknown verb"):
        playing.act("juggle", "trapdoor")
    with pytest.raises(RuntimeError, match="unknown target"):
        playing.act("look_at", "hippopotamus")

    # "use X on Y" needs one of the two to be something carried.
    scenery = [obj["name"] for obj in playing.state()["objects"]]
    assert len(scenery) >= 2, f"not enough on this screen to test with: {scenery}"
    with pytest.raises(RuntimeError, match="item you are carrying"):
        playing.act("use", scenery[0], scenery[1])
    with pytest.raises(RuntimeError, match="say what to use it on"):
        playing.act("use", "dart")

    # Nothing is asking a question, so there is nothing to answer.
    with pytest.raises(RuntimeError, match="no conversation question is pending"):
        playing.call_tool("answer", {"id": 1})


def test_10_sword2_debug_reads_the_engine(playing: McpClient) -> None:
    """The debug tool reports what the engine thinks, for diagnosis."""
    system = playing.call_tool("debug")["system"]

    assert system["can_act"] is True and system["mouse_available"] is True, system
    assert system["location"] > 0, system
    assert system["object_labels"] is True, (
        "the ini asks for object labels and the engine should have them on"
    )
    assert system["object_held"] == 0, system

    items = playing.call_tool("debug", {"items": True, "system": False})["items"]
    assert any(item["name"] == "boathook" for item in items), items

    variables = playing.call_tool(
        "debug", {"vars": True, "from": 62, "to": 62, "system": False}
    )["vars"]
    assert variables[0]["index"] == 62, variables


def test_11_sword2_raw_input_tools_are_accepted(playing: McpClient) -> None:
    """The raw input tools work here too, for an agent driving by screenshot."""
    playing.call_tool("mouse_move", {"x": 320, "y": 240})
    playing.call_tool("keystroke", {"key": "Escape"})

    result = playing.call_tool_raw("screenshot")
    structured = result["structuredContent"]
    assert structured["width"] == 640 and structured["height"] == 480, structured

    images = [b for b in result["content"] if b.get("type") == "image"]
    assert len(images) == 1, f"no image in the result: {result['content']}"
    assert base64.b64decode(images[0]["data"])[:4] == b"\x89PNG"
