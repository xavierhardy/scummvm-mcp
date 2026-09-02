"""The games whose engines had no MCP bridge until this work.

Four engines, eight games, and each engine answers a different question about
what "a room" even is:

  * **AGI** (King's Quest II and III, Police Quest) is a parser game. Nothing
    on screen is labelled and nothing is clicked - the player types. So there
    is no list of things in the room to check; what there is instead is the
    OBJECT file, which names every item in the game, and the parser's own
    dictionary, which is every word it understands. Both are checked here,
    because an agent with neither is guessing at a vocabulary of hundreds of
    words and cannot tell a typo from a wrong idea.

  * **Kyrandia** is a pointer game with no verb bar, and it labels nothing
    either. Its names come from its item table, and its four compass exits are
    named by the bridge because the engine never named them.

  * **AGOS** (Simon the Sorcerer) is the friendliest: every clickable thing
    carries the name the game writes along the bottom of the screen, and the
    verbs are a fixed bar of twelve.

  * **Sanitarium** has no verbs at all - the cursor changes shape to say what a
    click would do - and every object carries the name its authors typed.

What is common to all of them, and what most of these tests check, is the
contract every bridge in this repository owes: a room comes back, the names in
it are usable, a wrong name is refused with the right ones, and an action
reaches the game rather than hanging.
"""

import pytest

from mcp_client import McpClient
from state_helpers import wait_until_taking_input

#: fixture -> engine. The ids are the recorder catalogue's.
NEW_ENGINE_GAMES = [
    ("kq2", "agi"),
    ("kq3", "agi"),
    ("pq1", "agi"),
    ("kyra1", "kyra"),
    ("kyra2", "kyra"),
    ("kyra3", "kyra"),
    ("simon1", "agos"),
    ("sanitarium", "asylum"),
]

PARSER_GAMES = [g for g, engine in NEW_ENGINE_GAMES if engine == "agi"]
POINTER_GAMES = [g for g, engine in NEW_ENGINE_GAMES if engine != "agi"]

CORE_TOOLS = {"state", "act", "walk", "skip"}


def _client(request, fixture: str) -> McpClient:
    return request.getfixturevalue(f"{fixture}_client")


# ---------------------------------------------------------------------------
# True of every one of them
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "fixture,engine", NEW_ENGINE_GAMES, ids=[f for f, _ in NEW_ENGINE_GAMES]
)
def test_the_game_says_where_it_is(request, fixture: str, engine: str) -> None:
    state = wait_until_taking_input(_client(request, fixture), fixture)
    room = state.get("room")
    assert isinstance(room, dict), f"{fixture}: state() returned no room"
    assert "id" in room, f"{fixture}: the room has no id: {room}"


@pytest.mark.parametrize(
    "fixture,engine", NEW_ENGINE_GAMES, ids=[f for f, _ in NEW_ENGINE_GAMES]
)
def test_the_tools_are_registered(request, fixture: str, engine: str) -> None:
    offered = {tool["name"] for tool in _client(request, fixture).list_tools()}
    assert CORE_TOOLS <= offered, f"{fixture}: missing {CORE_TOOLS - offered}"


@pytest.mark.parametrize("fixture", POINTER_GAMES)
def test_a_target_that_is_not_here_is_refused(request, fixture: str) -> None:
    """A wrong guess has to come back as a refusal that names it, not silence.

    These are the engines where an agent is most likely to guess wrong - none
    of the three labels anything on screen - so the refusal is the whole of the
    feedback loop. It also has to arrive whatever the game is doing: a name the
    room does not have is wrong during a cutscene too, and answering "not
    accepting input" there would send the caller away to wait for a moment that
    was never going to help.
    """
    client = _client(request, fixture)
    wait_until_taking_input(client, fixture)
    with pytest.raises(RuntimeError) as caught:
        client.act("look_at", "no_such_thing_is_here")
    assert "no_such_thing_is_here" in str(caught.value), f"{fixture}: {caught.value}"


@pytest.mark.parametrize("fixture", PARSER_GAMES)
def test_a_parser_game_answers_a_word_it_does_not_know(request, fixture: str) -> None:
    """A parser game refuses nothing: it takes the sentence and replies.

    There is no list of things in the room to check a noun against - that is
    the whole difference between these games and the pointer ones - so a word
    the game has never heard is not an error, it is a turn. What matters is
    that the call completes and the game says something rather than the action
    hanging on a sentence that meant nothing to it.
    """
    client = _client(request, fixture)
    wait_until_taking_input(client, fixture)
    result = client.act("look_at", "no_such_thing_is_here")
    assert isinstance(result, dict), f"{fixture}: act returned {result!r}"


# ---------------------------------------------------------------------------
# The parser games
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("fixture", PARSER_GAMES)
def test_a_parser_game_names_the_items_that_exist(request, fixture: str) -> None:
    """AGI's OBJECT file is the one list of things the game has names for.

    Nothing on screen is labelled, so without this an agent has nothing at all
    to refer to.
    """
    state = wait_until_taking_input(_client(request, fixture), fixture)
    named = [obj.get("name") for obj in (state.get("objects") or [])]
    named += [obj.get("name") for obj in (state.get("inventory") or [])]
    assert named, f"{fixture}: the game named no items at all"
    assert all(isinstance(name, str) and name for name in named), (
        f"{fixture}: an item came back without a name: {named}"
    )


@pytest.mark.parametrize("fixture", PARSER_GAMES)
def test_a_parser_game_publishes_the_words_it_knows(request, fixture: str) -> None:
    """The dictionary is what makes a parser game playable rather than guessable.

    The interpreter answers a word it has never heard exactly as it answers a
    sensible idea it cannot carry out ("I don't know how to do that"), so an
    agent that cannot read the dictionary cannot tell those two apart.
    """
    client = _client(request, fixture)
    result = client.call_tool("vocabulary", {})
    total = result.get("total")
    assert isinstance(total, int) and total > 100, (
        f"{fixture}: the parser reported only {total} words"
    )
    words = result.get("words") or []
    assert "look" in words, f"{fixture}: the dictionary has no word for looking"


@pytest.mark.parametrize("fixture", PARSER_GAMES)
def test_a_parser_game_narrows_its_dictionary_on_request(request, fixture: str) -> None:
    """Several hundred words is a lot to read; the filter is how it is used."""
    client = _client(request, fixture)
    everything = client.call_tool("vocabulary", {})
    narrowed = client.call_tool("vocabulary", {"starts_with": "lo"})
    assert narrowed["total"] == everything["total"], (
        f"{fixture}: filtering changed how many words the parser knows"
    )
    assert len(narrowed["words"]) < len(everything["words"]), (
        f"{fixture}: filtering narrowed nothing"
    )
    assert all(word.startswith("lo") for word in narrowed["words"]), (
        f"{fixture}: the filter let something else through"
    )


@pytest.mark.parametrize("fixture", PARSER_GAMES)
def test_typing_reaches_the_parser(request, fixture: str) -> None:
    """type_text is not a fallback here - it is how the game is played."""
    client = _client(request, fixture)
    result = client.call_tool("type_text", {"text": "look"})
    assert result.get("text") == "look", f"{fixture}: {result}"


# ---------------------------------------------------------------------------
# The pointer games
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("fixture", POINTER_GAMES)
def test_a_pointer_game_names_something_to_click(request, fixture: str) -> None:
    state = wait_until_taking_input(
        _client(request, fixture), fixture, want_objects=True
    )
    names = [obj.get("name") for obj in (state.get("objects") or [])]
    assert names, f"{fixture}: nothing in the room to act on"
    assert all(isinstance(name, str) and name for name in names), (
        f"{fixture}: an object came back without a name: {names}"
    )


@pytest.mark.parametrize("fixture", POINTER_GAMES)
def test_everything_named_can_be_aimed_at(request, fixture: str) -> None:
    """A name with no coordinates behind it is a name that cannot be clicked."""
    state = wait_until_taking_input(
        _client(request, fixture), fixture, want_objects=True
    )
    for obj in state.get("objects") or []:
        assert "x" in obj and "y" in obj, (
            f"{fixture}: {obj['name']} has nowhere to click"
        )


def test_simon_offers_the_whole_verb_bar(simon1_client: McpClient) -> None:
    """AGOS is the one engine here with a real verb bar, and all of it is offered.

    An agent that has to guess which of "get", "take" and "pick up" this game
    takes will spend its turns finding out.
    """
    verbs = wait_until_taking_input(simon1_client, "simon1").get("verbs") or []
    assert len(verbs) == 12, f"the bar should have twelve buttons, got {verbs}"
    for expected in ("walk_to", "look_at", "use", "talk_to", "give"):
        assert expected in verbs, f"{expected} missing from {verbs}"


def test_kyrandia_names_the_ways_out(kyra1_client: McpClient) -> None:
    """The exits have no names in the data at all, so the bridge gives them some.

    Without them a room is a dead end: there is nothing else in a Kyrandia
    scene that leaves it.
    """
    names = [
        obj["name"]
        for obj in (
            wait_until_taking_input(kyra1_client, "kyra1", want_objects=True).get(
                "objects"
            )
            or []
        )
    ]
    exits = [name for name in names if name.startswith("exit_")]
    assert exits, f"no way out of the first scene: {names}"
    assert all(
        name in ("exit_north", "exit_east", "exit_south", "exit_west") for name in exits
    ), f"an exit was named something unexpected: {exits}"


def test_sanitarium_keeps_the_editors_filler_out_of_the_room(
    sanitarium_client: McpClient,
) -> None:
    """The game ships hundreds of nameless objects; none of them is a target.

    They are scenery the scripts push around, and offering them to an agent is
    offering it noise to work through.
    """
    state = wait_until_taking_input(sanitarium_client, "sanitarium", want_objects=True)
    names = [obj["name"] for obj in (state.get("objects") or [])]
    assert names, "nothing named in the first room"
    for name in names:
        assert not name.isdigit(), f"{name} is a slot number, not a name"
        assert name not in ("x", "xx", "xxx", "none"), f"{name} is filler"
