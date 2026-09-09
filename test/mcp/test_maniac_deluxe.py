"""
Integration tests for Maniac Mansion Deluxe (AGS engine, opening kid selection).

The AGS remake opens where the 1987 original does: a row of seven portraits, a
START button and a line asking for two more kids, with Dave picked already.
Nothing on that screen has a name, so a plain room snapshot of it is empty and
the whole thing is reached through choose_kids — the same tool, and the same
answers, as the original game's bridge offers (test_maniac_full.py), which is
the point: an agent that has played one has learnt the other.

There is no save to start from, so the sequence runs in order on one
session-scoped instance and is pinned to a single xdist worker.

The screen also has a clock on it: left alone for a minute the game starts
playing itself, which is a minute an agent may well spend thinking. So the
first test spends it, and the walkthrough starts from there.

Walkthrough: the opening screen advertises itself through state's
kid_selection_pending flag, and goes on doing so once the game has wandered off
into its own demo -> choose_kids escapes back to the screen, names the two kids
joining Dave, clicks their portraits, presses START and escapes through the
intro -> the game is running outside the mansion with the chosen team on
screen, and control can be handed round the three of them.
"""

import time

import pytest

from utils import McpClient, object_names

pytestmark = [pytest.mark.xdist_group("maniac_deluxe")]

KIDS = ["bernard", "razor"]

#: Outside the mansion: where the intro leaves the team.
FRONT_GATE = 1

#: The opening screen's room, and how long to wait for the game to wander out
#: of it on its own - it takes about a minute.
OPENING_SCREEN = 50
ATTRACT_TRIES = 24


def test_01_maniac_deluxe_opening_screen(maniac_deluxe_client: McpClient) -> None:
    """The opening screen is flagged as waiting for the kid selection."""
    state = maniac_deluxe_client.state()
    assert state.get("kid_selection_pending") is True, (
        f"expected the opening screen's kid selection, got {state}"
    )
    # Nothing on it can be acted on: the portraits are unnamed hotspots over a
    # painted title, and the game hides its interface until play begins.
    assert not state.get("objects"), f"unexpected things on the opening screen: {state}"
    assert state.get("can_act") is False, f"the opening screen is not playable: {state}"


def test_02_maniac_deluxe_waits_out_the_games_own_demo(
    maniac_deluxe_client: McpClient,
) -> None:
    """Left alone, the game plays itself - and is still waiting to be told.

    This is the minute an agent spends on its first turn, so it is spent here
    too: what follows is the screen as an agent that thought about it actually
    finds it, not as it is a second after boot.
    """
    for _ in range(ATTRACT_TRIES):
        if maniac_deluxe_client.state()["room"]["id"] != OPENING_SCREEN:
            break
        time.sleep(5)
    state = maniac_deluxe_client.state()
    assert state["room"]["id"] != OPENING_SCREEN, (
        f"the game never started showing itself off: {state}"
    )
    # The demo is the game with nobody in charge: no team, nothing to act on,
    # and the same thing still being asked for.
    assert state.get("kid_selection_pending") is True, f"still nobody picked: {state}"
    assert state.get("available_characters") == ["dave"], f"a team already: {state}"
    assert state.get("can_act") is False, f"the demo is not playable: {state}"


def test_03_maniac_deluxe_choose_kids_starts_game(
    maniac_deluxe_client: McpClient,
) -> None:
    """choose_kids picks the team by name and leaves the player in control."""
    result = maniac_deluxe_client.choose_kids(KIDS)
    assert result.get("kids") == ["dave"] + KIDS, f"unexpected team: {result}"

    state = maniac_deluxe_client.state()
    assert "kid_selection_pending" not in state, (
        f"opening screen still up after choose_kids: {state}"
    )
    # The game proper: the intro has been escaped through, the verb bar is back
    # and the two chosen kids stand outside the mansion with Dave.
    assert state["room"]["id"] == FRONT_GATE, f"not outside the mansion: {state}"
    assert state.get("can_act") is True, f"the player is not in control: {state}"
    names = object_names(state)
    for kid in KIDS:
        assert kid in names, f"{kid} is not in the room: {sorted(names)}"


def test_04_maniac_deluxe_choose_kids_rejected_in_game(
    maniac_deluxe_client: McpClient,
) -> None:
    """Once the game has started there is no selection left to make."""
    with pytest.raises(RuntimeError, match="already in the mansion"):
        maniac_deluxe_client.choose_kids(["wendy", "jeff"])


def test_05_maniac_deluxe_team_is_named(maniac_deluxe_client: McpClient) -> None:
    """state names the three kids in play and which of them has control."""
    state = maniac_deluxe_client.state()
    assert state.get("controlling") == "dave", f"Dave starts in control: {state}"
    available = state.get("available_characters")
    assert available == ["dave"] + KIDS, f"unexpected team: {available}"


def test_06_maniac_deluxe_switch_character(maniac_deluxe_client: McpClient) -> None:
    """Control goes round the team, whichever portrait button carries whom.

    The verb bar shows the two kids who are *not* being controlled, so which
    button is which changes with every press — asking for each of them in turn
    is what proves the tool is reading the answer back rather than counting.
    """
    for kid in KIDS + ["dave"]:
        result = maniac_deluxe_client.switch_character(kid)
        assert result.get("controlling") == kid, f"{kid} did not take control: {result}"
        assert maniac_deluxe_client.state().get("controlling") == kid


def test_07_maniac_deluxe_switch_character_rejects_a_stranger(
    maniac_deluxe_client: McpClient,
) -> None:
    """A kid who was never picked is not on the team and cannot be switched to."""
    with pytest.raises(RuntimeError, match="nobody on the team"):
        maniac_deluxe_client.switch_character("jeff")
