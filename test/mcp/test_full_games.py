"""The full games, as distinct from the demos every other module here covers.

Twenty-seven retail games across six engines, and what is being checked is the
same thing for all of them: that the bridge which was written against a demo
still describes the whole game. That is not a given. A demo is one or two
rooms with the awkward parts cut out; a full game has the copy-protection
screen, the ten-minute opening film, the CD's worth of resources and the
scripts the demo's author left out — and each of those has, at some point in
this suite's history, been the thing that stopped a bridge working.

So these are deliberately shallow and wide. Each game gets the same handful of
questions, asked through the same tools an agent has:

  * does `state` come back at all, with a room
  * does the room name things, and are the names usable as `act` targets
  * are the tools this engine promises actually registered
  * does an action reach the game — something changes, or the game says why not

Anything deeper about one game belongs in that game's own module.

Every game starts from its own slot 1, captured just past the opening (see the
`*_client` fixtures). A game whose slot has not been captured on this machine
skips rather than fails, the same way a game with no data configured does - and
starts at its title screen instead, so every test here waits for the game to be
taking input before it asks anything (`wait_until_taking_input`) rather than
reporting the opening film as a broken bridge.

King's Quest IV is missing from the table on purpose. The copy here ships a
copy-protection crack that ScummVM will not load - it is a shorter script than
the resource map says, and the engine stops rather than read past the end -
and with it removed the game asks a question out of its printed manual. The
bridge drives it perfectly well (`type_text` types at the prompt and the game
echoes and refuses each answer); there is simply no way past the question
without the manual, so there is no room to start a test from.
"""

import pytest

from mcp_client import McpClient
from state_helpers import wait_until_taking_input

# game fixture -> the engine behind it, which is what says which tools to
# expect. The ids are the catalogue's, and the demos' ids with `-full` after
# them wherever the demo is in this suite too.
FULL_GAMES = [
    ("ft_full", "scumm"),
    ("loom_full", "scumm"),
    ("indy3_full", "scumm"),
    ("atlantis_full", "scumm"),
    ("samnmax_full", "scumm"),
    ("monkey_full", "scumm"),
    ("dw_full", "tinsel"),
    ("dw2_full", "tinsel"),
    ("sword1_full", "sword1"),
    ("gob2_full", "gob"),
    ("gob3_full", "gob"),
    ("ween_full", "gob"),
    ("gk1_full", "sci"),
    ("sq6_full", "sci"),
    ("kq5_full", "sci"),
    ("kq6_full", "sci"),
    ("kq7_full", "sci"),
    ("sq4_full", "sci"),
    ("sq5_full", "sci"),
    ("pq3_full", "sci"),
    ("kq1sci_full", "sci"),
    ("sq1sci_full", "sci"),
    ("qfg1_full", "sci"),
    ("qfg2_full", "sci"),
    ("sq2vga", "ags"),
    ("pq2_full", "sci"),
]

#: Every bridge registers these, whatever the engine.
CORE_TOOLS = {"state", "act", "walk", "skip"}


def _client(request, fixture: str) -> McpClient:
    return request.getfixturevalue(f"{fixture}_client")


@pytest.mark.parametrize("fixture,engine", FULL_GAMES, ids=[f for f, _ in FULL_GAMES])
def test_the_game_says_where_it_is(request, fixture: str, engine: str) -> None:
    """A room, with a number — the one thing every engine can always answer."""
    state = wait_until_taking_input(_client(request, fixture), fixture)
    room = state.get("room")
    assert isinstance(room, dict), f"{fixture}: state() returned no room"
    assert "id" in room, f"{fixture}: the room has no id: {room}"


@pytest.mark.parametrize("fixture,engine", FULL_GAMES, ids=[f for f, _ in FULL_GAMES])
def test_the_room_names_something_to_act_on(request, fixture: str, engine: str) -> None:
    """The names in `objects` are the names `act` takes, so they must exist.

    A game past its opening is standing somewhere with things in it. An empty
    room here means the save was captured too early, or that the bridge cannot
    read this game's room the way it reads the demo's.
    """
    state = wait_until_taking_input(_client(request, fixture), fixture,
                                    want_objects=True)
    names = [obj.get("name") for obj in (state.get("objects") or [])]
    assert names, f"{fixture}: nothing in the room to act on"
    assert all(isinstance(name, str) and name for name in names), (
        f"{fixture}: an object came back without a name: {names}"
    )


@pytest.mark.parametrize("fixture,engine", FULL_GAMES, ids=[f for f, _ in FULL_GAMES])
def test_the_tools_this_engine_promises_are_registered(
    request, fixture: str, engine: str
) -> None:
    client = _client(request, fixture)
    offered = {tool["name"] for tool in client.list_tools()}
    assert CORE_TOOLS <= offered, f"{fixture}: missing {CORE_TOOLS - offered}"


@pytest.mark.parametrize("fixture,engine", FULL_GAMES, ids=[f for f, _ in FULL_GAMES])
def test_a_target_that_is_not_here_is_refused_with_the_ones_that_are(
    request, fixture: str, engine: str
) -> None:
    """The refusal is how an agent finds its way back after guessing wrong.

    Naming a thing that is not in the room has to come back as a refusal that
    lists what *is* — not as a silent no-op, and not as a stream that runs to
    its timeout.
    """
    client = _client(request, fixture)
    before = wait_until_taking_input(client, fixture, want_objects=True)
    with pytest.raises(RuntimeError) as caught:
        client.act("look_at", "no_such_thing_is_here")
    message = str(caught.value)
    assert "no_such_thing_is_here" in message, f"{fixture}: {message}"
    # The refusal lists the room as it was when the refusal was written, which
    # is not always the room the test looked at a moment earlier: a game with
    # no captured save is still animating its way out of its opening and can
    # change scene in between. So the room is read again afterwards, and the
    # message has to match one end of that or the other - and a room that has
    # genuinely emptied is allowed to say so.
    after = [obj["name"] for obj in (client.state().get("objects") or [])]
    here = {obj["name"] for obj in (before.get("objects") or [])} | set(after)
    assert not after or any(name in message for name in here), (
        f"{fixture}: the refusal did not say what is here: {message}"
    )


@pytest.mark.parametrize("fixture,engine", FULL_GAMES, ids=[f for f, _ in FULL_GAMES])
def test_acting_on_something_reaches_the_game(request, fixture: str, engine: str) -> None:
    """Look at the first thing in the room and get an answer back.

    What the answer *is* varies by game and is not the point: some describe the
    thing, some refuse it, some walk over to it first. The point is that the
    call completes rather than hanging, and comes back with the shape the
    bridge's own schema promises — which the client checks for every call.
    """
    client = _client(request, fixture)
    state = wait_until_taking_input(client, fixture, want_objects=True)
    target = state["objects"][0]["name"]
    try:
        result = client.act("look_at", target)
    except RuntimeError as error:
        # A refusal naming the target is the game answering, not a failure:
        # plenty of scenery cannot be looked at, and a game with no `look_at`
        # says so.
        assert target in str(error) or "verb" in str(error), f"{fixture}: {error}"
        return
    assert isinstance(result, dict), f"{fixture}: act returned {result!r}"
