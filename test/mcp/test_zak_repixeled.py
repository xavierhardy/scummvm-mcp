"""
Integration test for Zak McKracken - repixeled (AGS engine, verbs on a bar).

AGS is a toolkit, so everything in a room was named by its author at design
time and both names are still there at run time. That is what makes this
bridge's snapshot different from every other one here: nothing has to be swept
for with a cursor, and a room comes back with the author's own words in it.

The other half is the verb, and this game is the awkward shape. Most AGS games
make the verb the cursor mode, which act() can set outright. This one builds a
SCUMM-style bar out of GUI buttons and drives its own verb state from them, so
set_cursor_mode means nothing to it - press Look with it and the status line
still reads "walk to". Which button is which verb is written nowhere either:
the buttons still carry the editor's default text and the game draws its own
labels. So an agent presses a button and reads back what the game then says is
selected, which is what select_verb is.

No save support in the demo sense - the game autosaves but offers no slot to
start from - so this is one ordered sequence on a single fresh instance.
"""

import time

import pytest

from mcp_client import McpClient

pytestmark = [pytest.mark.xdist_group("zak_repixeled")]

BEDROOM = 5


def _into_the_bedroom(client: McpClient, tries: int = 30) -> dict:
    """Skip the opening until Zak's own room is up.

    The pause matters: the opening is a run of scripted screens, and asking
    again immediately just spends the whole budget inside the first.
    """
    for _ in range(tries):
        state = client.state()
        # Being in the room is not the same as being handed it: the scene
        # plays itself out for a while after it opens.
        if (state.get("room") or {}).get("id") == BEDROOM and state.get("can_act"):
            return state
        try:
            client.skip()
        except RuntimeError as exc:
            if "starting up" not in str(exc) and "nothing to skip" not in str(exc):
                raise
        time.sleep(2)
    raise AssertionError("the opening never gave way to a bedroom Zak controls")


@pytest.fixture(scope="session")
def playing(zak_repixeled_client: McpClient) -> McpClient:
    _into_the_bedroom(zak_repixeled_client)
    return zak_repixeled_client


@pytest.fixture(scope="session")
def bar(playing: McpClient) -> dict:
    """The verb bar, learned the way an agent learns it: press each button
    once and read back what the game says is selected."""
    # Only the run of buttons at the left of the bar. The bridge offers every
    # button on the displayed GUIs, because it cannot know which of them is a
    # verb - and one of the others here is the settings cog, which opens a
    # panel over the status line and leaves nothing to read.
    learned: dict[str, int] = {}
    playing.call_tool("select_verb", {"button": 0})
    for button in range(5):
        playing.call_tool("select_verb", {"button": button})
        # The game writes its status line from its own script, on a later
        # loop, so what the press made of the verb is not readable at once.
        time.sleep(1.5)
        verb = playing.state().get("current_verb")
        if verb and verb not in learned:
            learned[verb] = button
    return learned


def _select(client: McpClient, verb: str, bar: dict) -> bool:
    """Get *verb* selected, the way an agent would.

    The remembered button first; if the game does not end up showing that
    verb - a press can land while the game is busy with something else - the
    others are tried in turn. Which button is which never changes, so this
    converges rather than wanders.
    """
    known = bar.get(verb)
    order = ([known] if known is not None else []) + [b for b in range(5) if b != known]
    for button in order:
        client.call_tool("select_verb", {"button": button})
        if _verb_now(client, tries=4) == verb:
            return True
    return False


def _verb_now(client: McpClient, tries: int = 6) -> str | None:
    """The selected verb, given a few goes: the game writes its status line
    from its own script, on a later loop than the press."""
    for _ in range(tries):
        verb = client.state().get("current_verb")
        if verb:
            return verb
        time.sleep(1)
    return None


def test_01_the_room_carries_its_authors_names(playing: McpClient) -> None:
    names = {o["name"] for o in playing.state()["objects"]}
    # A handful the apartment demonstrably has. Not the whole list: what is on
    # screen changes as the scene plays.
    for expected in ("cat_clock", "torn_wallpaper", "plastic_card"):
        assert expected in names, f"{expected} missing from {sorted(names)}"


def test_02_the_editors_own_placeholders_are_not_offered(playing: McpClient) -> None:
    """A hotspot nobody named is still "Hotspot 4" in the data."""
    names = {o["name"] for o in playing.state()["objects"]}
    assert not any(n.startswith("hotspot_") for n in names), sorted(names)
    assert not any(n.startswith("object_") for n in names), sorted(names)


def test_03_everything_named_has_somewhere_to_click(playing: McpClient) -> None:
    """A hotspot is an area painted into a mask, not a rectangle, so most of
    them have no position in the data at all - the bridge finds one."""
    objects = playing.state()["objects"]
    hotspots = [o for o in objects if o["kind"] == "hotspot"]
    assert hotspots, "no hotspots in the room at all"
    for obj in objects:
        assert obj["x"] > 0 or obj["y"] > 0, f"{obj} has nowhere to click"


def test_04_the_three_kinds_are_told_apart(playing: McpClient) -> None:
    kinds = {o["kind"] for o in playing.state()["objects"]}
    assert kinds <= {"object", "hotspot", "character"}, kinds
    assert "object" in kinds and "hotspot" in kinds


def test_05_state_lists_the_bars_verbs(playing: McpClient) -> None:
    verbs = playing.state()["verbs"]
    for expected in ("look_at", "use", "take", "talk_to"):
        assert expected in verbs, verbs


def test_06_pressing_a_button_says_what_it_meant(bar: dict) -> None:
    """This is the whole of how a verb bar is learned, and it has to work
    before anything can be acted on with a chosen verb."""
    assert "look_at" in bar, f"nothing on the bar turned out to be a look: {bar}"
    assert len(bar) >= 4, f"the bar barely said anything: {bar}"


def test_07_asking_by_name_says_why_it_cannot(playing: McpClient) -> None:
    """These buttons never say what they are, so a name is not something the
    bridge can resolve - and it says so rather than pressing one at random."""
    result = playing.call_tool("select_verb", {"verb": "look_at"})
    assert "error" in result, result
    assert "by number" in result["error"], result


def test_08_acting_on_a_named_thing_makes_zak_say_something(
    playing: McpClient,
) -> None:
    """The whole sequence, as an agent plays it: choose a verb on the bar, act
    on a thing the room named, and read what was said off the action's own
    stream - the lines arrive there as notifications and are gone from the
    next snapshot by the time it is asked for.

    Every button is tried rather than the one previously found to be a look:
    which of them is which is the agent's to remember, a press that lands
    while the game is busy is simply lost, and what is being checked here is
    that acting produces speech at all.
    """
    for button in range(5):
        playing.call_tool("select_verb", {"button": button})
        time.sleep(1.5)
        playing.state()
        try:
            playing.act(verb=playing.state()["verbs"][1], target1="cat_clock")
        except RuntimeError:
            continue
        said = [m.get("text", "") for m in playing.last_notifications]
        said += [m["text"] for m in playing.state().get("messages", [])]
        if any(len(line.strip()) > 4 for line in said):
            return
    raise AssertionError("acting on the clock never produced a line of speech")


def test_09_a_target_that_is_not_here_is_refused_with_the_ones_that_are(
    playing: McpClient,
) -> None:
    with pytest.raises(RuntimeError) as excinfo:
        playing.act(verb="look_at", target1="flying_saucer")
    message = str(excinfo.value)
    assert "flying_saucer" in message
    assert "cat_clock" in message


def test_10_walking_moves_zak(playing: McpClient, bar: dict) -> None:
    """Walking is not on the bar - Look, Use, Pick up, Talk and Give are, and
    walking is what a click means when none of them is chosen. The game falls
    back to it once an action has been carried out, so this waits for it to
    rather than pressing anything.
    """
    assert "walk_to" not in bar, f"walk turned out to be a button after all: {bar}"
    for _ in range(6):
        if playing.state().get("current_verb") != "walk_to":
            time.sleep(2)
            continue
        before = playing.state()["position"]
        playing.walk(200 if before["x"] < 180 else 100, 137)
        after = playing.state()["position"]
        if after != before:
            return
    raise AssertionError("the game never came back to walking, so nothing moved")
