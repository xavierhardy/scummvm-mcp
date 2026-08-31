"""
Integration test for the Toonstruck demo (Toon engine).

Toonstruck is a one-click cartoon adventure: no verb bar, no inventory screen
worth opening — a left click is "do whatever this thing is for", a right click
is "say something about it", and what is carried is one item at a time, in the
hand. The bridge maps that onto the same three verbs every other adapter
offers, and drives it by putting the pointer where the target is and pressing,
so the game resolves the click itself.

There is no save to load: the demo opens on a logo movie and drops the player
at the crossroads, so the whole suite is one ordered walkthrough on a single
fresh instance. The route it takes is the demo's own: the crossroads, the
street outside the gym, the parade of Wacme entrances (which is wider than the
screen, and so is where scrolling is checked), and the conversation the
outhouse guard starts.
"""

import time

import pytest

from mcp_client import McpClient

# One ordered walkthrough on a single instance — pin it to one xdist worker.
pytestmark = [pytest.mark.xdist_group("toon")]

# The scene the demo starts in, and the two it leads to.
CROSSROADS = "zcross"
GYM_STREET = "jimex"
WACME_ROW = "wacexdbl"


def _wait_idle(client: McpClient, tries: int = 40) -> dict:
    """The state once the game is accepting input again."""
    for _ in range(tries):
        state = client.state()
        if state.get("can_act"):
            return state
        time.sleep(0.5)
    raise AssertionError("the game never went back to accepting input")


def _skip_intro(client: McpClient, max_skips: int = 40) -> dict:
    """Get past the opening movie; return the first playable state."""
    for _ in range(max_skips):
        state = client.state()
        if state.get("can_act") and state.get("objects"):
            return state
        try:
            client.skip()
        except RuntimeError as exc:  # a skip with nothing to cut short
            if "starting up" not in str(exc):
                raise
            time.sleep(0.5)
    raise AssertionError("the opening never gave way to a playable scene")


def _system(client: McpClient) -> dict:
    return client.call_tool("debug")["system"]


def _scene(client: McpClient) -> str:
    return client.state()["room"]["name"]


def _named(client: McpClient, name: str) -> dict:
    for obj in client.state()["objects"]:
        if obj["name"] == name:
            return obj
    raise AssertionError(f"{name} is not in {_scene(client)}")


def _go_to(client: McpClient, way_out: str, scene: str) -> None:
    """Take a way out and wait until the scene it leads to has settled."""
    if _scene(client) == scene:
        return
    client.act("use", way_out)
    _wait_idle(client)
    assert _scene(client) == scene, (
        f"'{way_out}' was supposed to lead to {scene}, not {_scene(client)}"
    )


@pytest.fixture(scope="session")
def playing(toon_client: McpClient) -> McpClient:
    """The demo, past the opening and accepting input."""
    _skip_intro(toon_client)
    return toon_client


def test_01_toon_state_describes_the_scene(playing: McpClient) -> None:
    """The snapshot is the scene as the game labels it, not as pixels."""
    state = playing.state()

    assert state["room"]["name"] == CROSSROADS, f"not where the demo starts: {state}"
    assert state["can_act"] is True, f"the game is not accepting input: {state}"
    assert state["verbs"] == ["use", "look_at", "walk_to"], f"unexpected verbs: {state}"

    assert state["objects"], f"nothing to point at in this scene: {state}"
    for obj in state["objects"]:
        assert obj["name"], f"an unnamed target: {obj}"
        assert obj["kind"] in ("character", "scenery"), obj
        # Scene coordinates: a scene can be several screens wide, so only the
        # vertical extent is bounded.
        assert obj["x"] >= 0 and 0 <= obj["y"] < 400, f"outside the scene: {obj}"

    names = [obj["name"] for obj in state["objects"]]
    assert len(names) == len(set(names)), f"two targets share a name: {names}"

    # The sidekick is a target in his own right, named the way the game names
    # him rather than as a numbered thing.
    characters = [obj for obj in state["objects"] if obj["kind"] == "character"]
    assert characters, f"the sidekick is not pointable: {state['objects']}"
    assert all(not obj["name"].startswith("character_") for obj in characters), (
        f"a character the game names came out numbered: {characters}"
    )

    # Where the player character stands is a point in the scene, and it is the
    # one the engine itself holds.
    system = _system(playing)
    assert 0 <= state["position"]["x"] < system["scene_width"], state
    assert 0 <= state["position"]["y"] < 400, state
    assert (state["position"]["x"], state["position"]["y"]) == (
        system["lead_x"],
        system["lead_y"],
    ), f"the snapshot and the engine disagree: {state['position']} vs {system}"


def test_02_toon_items_are_named_the_way_the_game_names_them(
    playing: McpClient,
) -> None:
    """Carried items carry the game's own word for them, not an index.

    The game labels nothing in the bag; the only place it ever says what an
    item is, is the line the player character speaks about it. That line is
    where the names come from, which is what makes them echoable back."""
    items = playing.state()["inventory"]
    assert items, f"the demo starts with a full bag: {items}"
    for item in items:
        assert item["name"], f"an unnamed item: {item}"
        assert not item["name"].startswith("item_"), (
            f"{item} fell back to a numbered name"
        )
    names = [item["name"] for item in items]
    assert len(names) == len(set(names)), f"two items share a name: {names}"


def test_03_toon_look_at_reports_what_is_said(playing: McpClient) -> None:
    """`look_at` is the right click: the character remarks on the thing aloud."""
    sidekick = next(
        obj for obj in playing.state()["objects"] if obj["kind"] == "character"
    )
    result = playing.act("look_at", sidekick["name"])

    lines = result.get("messages", [])
    assert lines, f"looking at {sidekick['name']} said nothing: {result}"
    for line in lines:
        assert line["text"], line
        assert line["actor"], f"a line nobody said: {line}"
    assert playing.state()["messages"] == [], "the lines were reported twice"


def test_04_toon_walk_moves_the_character(playing: McpClient) -> None:
    """walk() takes a point and the call waits for the walk to finish."""
    _wait_idle(playing)
    before = playing.state()["position"]

    for dx in (-120, 120, -60, 60):
        result = playing.walk(before["x"] + dx, before["y"])
        after = playing.state()["position"]
        if after != before:
            assert "position" in result, f"walk reported no position: {result}"
            assert result["position"] == after, (
                f"walk returned {result['position']} but he stands at {after}"
            )
            return
        _wait_idle(playing)
    pytest.fail(f"he never moved from {before}")


def test_05_toon_walking_onto_something_is_refused(playing: McpClient) -> None:
    """A point something covers is an action on that thing, not a walk.

    Going there would mean clicking it, which is what act() is for — so walk
    says so and names what is in the way rather than doing something else."""
    _wait_idle(playing)
    covered = playing.state()["objects"][0]

    with pytest.raises(RuntimeError, match="is covered by"):
        playing.walk(covered["x"], covered["y"])


def test_06_toon_walk_to_stops_beside_a_target(playing: McpClient) -> None:
    """'walk_to' goes to a thing without touching it.

    A click on the thing would act on it, so what walk_to aims at is the open
    ground the game itself would put the character on to act — which is why it
    ends up somewhere near the target rather than on it, and why it leaves the
    hand and the scene alone."""
    _wait_idle(playing)
    sidekick = next(
        obj for obj in playing.state()["objects"] if obj["kind"] == "character"
    )
    before = playing.state()

    result = playing.act("walk_to", sidekick["name"])
    after = playing.state()

    assert after["position"] != before["position"], (
        f"walk_to {sidekick['name']} never moved him: {result}"
    )
    assert "inventory_added" not in result, (
        f"walk_to picked something up instead of walking: {result}"
    )
    assert after["room"] == before["room"], f"walk_to left the scene: {result}"


def test_07_toon_act_rejects_what_it_cannot_do(playing: McpClient) -> None:
    """The errors say what to do instead, rather than failing silently."""
    target = playing.state()["objects"][0]["name"]

    with pytest.raises(RuntimeError, match="unknown verb"):
        playing.act("juggle", target)
    with pytest.raises(RuntimeError, match="unknown target"):
        playing.act("look_at", "hippopotamus")
    with pytest.raises(RuntimeError, match="must be an item you are carrying"):
        playing.act("use", target, "hippopotamus")
    with pytest.raises(RuntimeError, match="no conversation question is pending"):
        playing.answer(1)


def test_09_toon_an_item_can_be_put_in_the_hand(playing: McpClient) -> None:
    """'target2' is the item an action is carried out with.

    There is one hand and it holds one thing, so naming an item is what takes
    it out of the bag — and the result says what ended up in it."""
    _wait_idle(playing)
    item = playing.state()["inventory"][0]["name"]
    target = next(
        obj for obj in playing.state()["objects"] if obj["kind"] == "character"
    )

    result = playing.act("use", target["name"], item)
    assert result.get("held_item") == item, f"{item} never reached the hand: {result}"

    held = [entry for entry in playing.state()["inventory"] if entry.get("held")]
    assert [entry["name"] for entry in held] == [item], (
        f"the state disagrees about what is held: {held}"
    )

    # And an action without an item named empties it again.
    result = playing.act("look_at", target["name"])
    assert result.get("held_item") == "", f"the hand was not emptied: {result}"
    assert not [e for e in playing.state()["inventory"] if e.get("held")]


def test_10_toon_a_way_out_leads_somewhere_and_says_so(playing: McpClient) -> None:
    """Taking a pathway reports the scene it arrived in, by name."""
    _wait_idle(playing)
    ways_out = [obj for obj in playing.state()["objects"] if obj.get("pathway")]
    assert ways_out or _named(playing, "path"), "no way out of the crossroads"

    result = playing.act("use", "path_3")
    _wait_idle(playing)

    assert result.get("room_changed"), f"the scene never changed: {result}"
    assert result.get("room_name") == WACME_ROW, f"unexpected scene: {result}"
    assert _scene(playing) == WACME_ROW


def test_11_toon_reaches_a_target_off_screen(playing: McpClient) -> None:
    """This scene is wider than the screen, and its far end is still usable.

    The game acts on whatever it thinks is being pointed at, and the pointer is
    a screen object — so a target beyond the edge cannot be pointed at until
    the view has come to it. The bridge moves the view there first, which is
    what makes the far end of a scene reachable at all."""
    _wait_idle(playing)
    system = _system(playing)
    width = system["scene_width"]
    assert width > 640, f"this scene was supposed to be a wide one: {system}"

    view = system["view_x"]
    offscreen = [
        obj
        for obj in playing.state()["objects"]
        if obj["kind"] == "scenery" and not view <= obj["x"] < view + 640
    ]
    assert offscreen, f"nothing is off screen at view_x={view}"
    target = max(offscreen, key=lambda obj: abs(obj["x"] - view))

    playing.act("look_at", target["name"])

    after = _system(playing)
    assert after["view_x"] <= target["x"] < after["view_x"] + 640, (
        f"{target['name']} at x={target['x']} is still off screen: {after}"
    )
    # And the pointer ended up on it, which is what the game reads a click at.
    assert abs(after["cursor_x"] - target["x"]) <= 2, (
        f"the pointer never reached {target['name']}: {after}"
    )


def test_12_toon_a_conversation_is_a_question_with_named_choices(
    playing: McpClient,
) -> None:
    """Talking opens a question; the options are named by what they say."""
    _go_to(playing, "central_zanydu", CROSSROADS)
    _go_to(playing, "path", GYM_STREET)

    result = playing.act("use", "outhouse_security_guard")
    question = result.get("question") or playing.state().get("question")
    assert question, f"the guard never asked anything: {result}"

    choices = question["choices"]
    assert len(choices) >= 2, f"a conversation with nothing to pick: {choices}"
    assert [c["id"] for c in choices] == list(range(1, len(choices) + 1)), choices
    for choice in choices:
        assert choice["label"], f"an unlabelled option: {choice}"
        assert not choice["label"].startswith("topic_"), (
            f"{choice} fell back to a numbered name"
        )
    assert [c for c in choices if c.get("ends_conversation")], (
        f"no way out of the conversation: {choices}"
    )

    # What the guard said on the way in came back with the question.
    assert result.get("messages"), f"the conversation was silent: {result}"

    # While one is pending, an action is refused and says what to do instead.
    with pytest.raises(RuntimeError, match="use 'answer'"):
        playing.act("look_at", "outhouse_security_guard")
    with pytest.raises(RuntimeError, match="must be between 1 and"):
        playing.answer(len(choices) + 1)


def test_13_toon_answering_carries_the_conversation_on(playing: McpClient) -> None:
    """An answer plays the exchange out and hands back the next question."""
    question = playing.state()["question"]
    opening = [c for c in question["choices"] if not c.get("ends_conversation")][0]

    result = playing.answer(opening["id"])
    assert result.get("messages"), f"answering said nothing: {result}"
    assert result.get("question"), f"the conversation stopped dead: {result}"


def test_14_toon_the_last_choice_ends_the_conversation(playing: McpClient) -> None:
    """The option flagged as the way out is the one that closes it."""
    question = playing.state()["question"]
    way_out = [c for c in question["choices"] if c.get("ends_conversation")][0]

    playing.answer(way_out["id"])
    _wait_idle(playing)

    assert playing.state().get("question") is None, "still being asked something"
    assert _system(playing)["in_conversation"] is False


def test_15_toon_skip_is_accepted(playing: McpClient) -> None:
    """skip() cuts a line short; with nothing to cut it is still a no-op, not
    an error, so an agent can always reach for it."""
    _wait_idle(playing)
    result = playing.skip()
    assert isinstance(result, dict), result
    _wait_idle(playing)


def test_16_toon_debug_reads_the_engine(playing: McpClient) -> None:
    """The debug tool reports what the engine thinks, for diagnosis."""
    _wait_idle(playing)
    system = _system(playing)

    assert system["scene_name"] == _scene(playing), system
    assert system["can_act"] is True, system
    assert system["scene_width"] >= 640, system
    assert system["view_x"] >= 0, system
    assert system["frame_counter"] > 0, system

    objects = playing.call_tool("debug", {"objects": True, "system": False})["objects"]
    assert objects, f"the scene has no pointable things: {objects}"
    for obj in objects:
        assert obj["x1"] <= obj["x"] <= obj["x2"], obj
        assert obj["y1"] <= obj["y"] <= obj["y2"], obj

    items = playing.call_tool("debug", {"items": True, "system": False})["items"]
    assert items, f"the bag reads empty: {items}"
    assert all(item["description"] for item in items), items


def test_17_toon_the_pointer_can_be_driven_directly(playing: McpClient) -> None:
    """mouse_move speaks the coordinates everything else here speaks."""
    _wait_idle(playing)
    target = playing.state()["objects"][0]

    playing.call_tool("mouse_move", {"x": target["x"], "y": target["y"]})
    system = _system(playing)
    assert (system["cursor_x"], system["cursor_y"]) == (target["x"], target["y"]), (
        f"the pointer is not where it was put: {system}"
    )


def test_18_toon_screenshot_returns_the_frame(playing: McpClient) -> None:
    """A screenshot comes back as an image, at the game's own resolution."""
    structured = playing.call_tool_raw("screenshot")["structuredContent"]
    assert structured["width"] == 640, f"unexpected frame size: {structured}"
    assert structured["height"] == 400, f"unexpected frame size: {structured}"
