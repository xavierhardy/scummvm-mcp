"""
Integration test for Zak McKracken and the Lonely Sea Monster (AGS engine).

The same bridge as Zak repixeled, on the other of AGS's two shapes: this game
has no verb bar and makes the verb the engine's own cursor mode, which act()
can set outright in the same breath as the click. So what this covers that
test_zak_repixeled does not is that the ordinary path works - a game with no
bar registers no select_verb dance and needs none.

It is also the one that shows names come out of the game rather than out of
English: the authors were French, and the room comes back saying so.

No save support, so this is one ordered sequence on a fresh instance.
"""

import pytest

from mcp_client import McpClient

pytestmark = [pytest.mark.xdist_group("zak_seamonster")]


def _playable(client: McpClient, tries: int = 14) -> dict:
    for _ in range(tries):
        state = client.state()
        if len(state.get("objects") or []) > 3:
            return state
        try:
            client.skip()
        except RuntimeError as exc:
            if "starting up" not in str(exc) and "nothing to skip" not in str(exc):
                raise
    raise AssertionError("the demo never showed a room with things in it")


@pytest.fixture(scope="session")
def playing(zak_seamonster_client: McpClient) -> McpClient:
    _playable(zak_seamonster_client)
    return zak_seamonster_client


def test_01_the_room_names_what_is_in_it(playing: McpClient) -> None:
    objects = playing.state()["objects"]
    assert objects, "the room came back empty"
    names = {o["name"] for o in objects}
    # Every name is an identifier, whatever language it was typed in.
    for name in names:
        assert name == name.lower(), f"{name} was never folded"
        assert " " not in name and "'" not in name, name


def test_02_the_people_in_the_room_are_told_from_the_scenery(
    playing: McpClient,
) -> None:
    kinds = {o["name"]: o["kind"] for o in playing.state()["objects"]}
    characters = [n for n, k in kinds.items() if k == "character"]
    assert characters, f"nobody in the room at all: {kinds}"


def test_03_the_verbs_are_the_engines_cursor_modes(playing: McpClient) -> None:
    """No bar here: the verb is the cursor, and a game that switched a mode
    off does not get it offered."""
    verbs = playing.state()["verbs"]
    assert verbs, "no verbs at all"
    assert set(verbs) <= {"walk_to", "look_at", "use", "talk_to", "use_inv", "take"}
    for expected in ("look_at", "use", "talk_to"):
        assert expected in verbs, verbs


def test_04_a_game_without_a_bar_does_not_offer_select_verb(
    playing: McpClient,
) -> None:
    """The tool is registered for every AGS game, because at start-up there is
    no room to ask - but it refuses on a game that has no bar rather than
    pressing something at random."""
    result = playing.call_tool("select_verb", {"button": 0})
    assert "error" in result, result
    assert "verb bar" in result["error"], result


def test_05_a_verb_the_game_does_not_have_is_refused_by_name(
    playing: McpClient,
) -> None:
    with pytest.raises(RuntimeError) as excinfo:
        playing.act(verb="dance", target1="zak")
    assert "dance" in str(excinfo.value)


def test_06_a_stream_opens_and_closes(playing: McpClient) -> None:
    """What matters is that a streaming call comes back rather than hanging."""
    first = playing.state()["objects"][0]["name"]
    result = playing.act(verb="look_at", target1=first)
    assert "room" in result, result
