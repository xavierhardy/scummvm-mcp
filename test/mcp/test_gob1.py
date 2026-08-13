"""
Integration test for the Gobliiins interactive demo (Gob engine).

Gobliiins runs on the same engine as Woodruff but plays nothing like it: there
is no verb bar, no hover text and no per-object hotspot — the whole picture is
one click zone. What the engine does keep is the game's own model: three
goblins the player switches between, each with an ability, and a table of scene
objects. The bridge reports those, and the verb set is the cursor: a click does
whatever the cursor currently means, and the right button cycles it — so
`walk to` sends the controlled goblin somewhere, `interact` makes him use his
own ability there, and `pick up` makes him take what is there.

The demo has no save support, so the whole run is one ordered sequence on a
single fresh instance (like woodruff and the atlantis/ft demos), skipping the
intro first.
"""

import base64

import pytest

from mcp_client import McpClient

# One ordered walkthrough on a single instance — pin it to one xdist worker.
pytestmark = [pytest.mark.xdist_group("gob1")]

TEAM = ["picker", "fighter", "mage"]


def _skip_intro(client: McpClient, max_skips: int = 12) -> dict:
    """Skip the intro videos until a playable screen is up; return that state."""
    for _ in range(max_skips):
        try:
            client.skip()
        except RuntimeError as exc:
            if "nothing to skip" not in str(exc):
                raise
        state = client.state()
        room = state.get("room") or {}
        if state.get("can_act") and room.get("name") not in (None, "", "intro"):
            return state
    raise AssertionError("the intro never gave way to a playable screen")


@pytest.fixture(scope="session")
def playing(gob1_client: McpClient) -> McpClient:
    """The demo, past the intro and accepting input."""
    _skip_intro(gob1_client)
    return gob1_client


def test_01_gob1_state_reports_the_team(playing: McpClient) -> None:
    """The snapshot is the game's own model: three characters and one in control."""
    state = playing.state()

    names = [c["name"] for c in state["characters"]]
    assert names == TEAM, f"unexpected team: {state}"
    assert state["controlling"] in TEAM, f"nobody is being controlled: {state}"
    for character in state["characters"]:
        assert character["x"] > 0 and character["y"] > 0, (
            f"{character['name']} has no position: {character}"
        )

    # Three verbs, one per cursor setting; no dialog questions in this game.
    assert state["verbs"] == ["walk to", "interact", "pick up"], (
        f"unexpected verbs: {state}"
    )
    assert "question" not in state, f"unexpected dialog question: {state}"

    # The scene objects come from the engine's table, each with a spot to click.
    assert state["objects"], f"no objects on screen: {state}"
    for obj in state["objects"]:
        assert obj["name"] == f"object_{obj['id']}", f"unexpected object: {obj}"
        assert 0 <= obj["x"] < 320 and 0 <= obj["y"] < 200, f"off screen: {obj}"


def test_02_gob1_switch_character(playing: McpClient) -> None:
    """switch_character hands control to another goblin, by name."""
    result = playing.call_tool("switch_character", {"name": "mage"})
    assert result.get("controlling") == "mage", f"switch refused: {result}"
    assert playing.state()["controlling"] == "mage"

    refused = playing.call_tool("switch_character", {"name": "plumber"})
    assert "unknown character" in refused.get("error", ""), (
        f"a name nobody has was accepted: {refused}"
    )

    playing.call_tool("switch_character", {"name": "picker"})
    assert playing.state()["controlling"] == "picker"


def test_03_gob1_walk_moves_the_controlled_goblin(playing: McpClient) -> None:
    """walk() sends the goblin in control across the screen."""
    controlling = playing.state()["controlling"]
    before = _position(playing)

    result = playing.walk(150, 150)
    assert result.get("controlling") == controlling, f"control changed: {result}"
    assert "position" in result, f"walk reported no position: {result}"

    after = _position(playing)
    assert after != before, f"the goblin never moved (still at {after})"
    # The call waits for the walk to finish, so what it reports is where he is,
    # not where he happened to be mid-stride.
    assert (result["position"]["x"], result["position"]["y"]) == after, (
        f"walk returned {result['position']} but he stands at {after}"
    )


def test_04_gob1_interact_acts_where_it_is_told(playing: McpClient) -> None:
    """`interact` takes a plain point — most of the picture is not an object."""
    before = _position(playing)
    result = playing.act("interact", x=250, y=150)
    assert "position" in result, f"interact reported nothing: {result}"
    assert _position(playing) != before, f"interact did nothing at all: {result}"

    # And a named object works the same way.
    target = playing.state()["objects"][0]["name"]
    result = playing.act("interact", target)
    assert "position" in result, f"interact on {target} reported nothing: {result}"


def test_05_gob1_the_verb_sets_the_cursor(playing: McpClient) -> None:
    """Each verb is a cursor setting, and the click carries that one out.

    This is the whole difference between walking somewhere and doing something
    there: the game reads its cursor mode as the action for the click, so a
    verb that never touched the cursor would only ever move the goblin. The
    mode is the game's own variable, which `debug` can read back."""
    for verb, expected in (("walk to", 0), ("interact", 3), ("pick up", 4)):
        playing.act(verb, x=200, y=140)
        assert _cursor_mode(playing) == expected, (
            f"'{verb}' left the cursor at {_cursor_mode(playing)}, not {expected}"
        )

    # walk() is a walk whatever the last action left the cursor on.
    playing.act("interact", x=200, y=140)
    playing.walk(150, 150)
    assert _cursor_mode(playing) == 0, "walk did not put the cursor back to walking"


def test_06_gob1_act_rejects_what_it_cannot_do(playing: McpClient) -> None:
    """The errors say what to do instead, rather than failing silently."""
    with pytest.raises(RuntimeError, match="unknown verb"):
        playing.act("talk to the hand", x=10, y=10)
    with pytest.raises(RuntimeError, match="unknown target"):
        playing.act("interact", "object_999")
    with pytest.raises(RuntimeError, match="off screen"):
        playing.act("interact", x=9999, y=10)


def test_07_gob1_screenshot_returns_the_frame(playing: McpClient) -> None:
    """A screenshot comes back as an image, which is how a game like this is read."""
    result = playing.call_tool_raw("screenshot")

    structured = result["structuredContent"]
    assert structured["width"] == 320 and structured["height"] == 200, (
        f"unexpected frame size: {structured}"
    )

    images = [b for b in result["content"] if b.get("type") == "image"]
    assert len(images) == 1, f"no image in the result: {result['content']}"
    assert images[0]["mimeType"] == "image/png"
    raw = base64.b64decode(images[0]["data"])
    assert raw[:4] == b"\x89PNG", "the image is not a PNG"


def test_08_gob1_cursor_tools_are_accepted(playing: McpClient) -> None:
    """The raw input tools work here too, for an agent driving by screenshot."""
    playing.call_tool("mouse_move", {"x": 160, "y": 100})
    playing.call_tool("mouse_click", {"x": 160, "y": 120, "button": "right"})
    playing.call_tool("keystroke", {"key": "Escape"})
    assert playing.state()["controlling"] in TEAM


# The game's own cursor-mode variable: 0 walk there, 3 act there, 4 take/put.
_CURSOR_MODE_VAR = 111


def _cursor_mode(client: McpClient) -> int:
    """What the game's cursor currently means."""
    result = client.call_tool(
        "debug",
        {
            "vars": True,
            "from": _CURSOR_MODE_VAR,
            "to": _CURSOR_MODE_VAR,
            "system": False,
        },
    )
    return int(result["vars"][0]["value"])


def _position(client: McpClient) -> tuple[int, int]:
    """Where the controlled goblin currently stands."""
    state = client.state()
    for character in state["characters"]:
        if character["name"] == state["controlling"]:
            return character["x"], character["y"]
    raise AssertionError(f"nobody is in control: {state}")
