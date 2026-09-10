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
screen, and control can be handed round the three of them -> out of the gate
and the length of the front garden, which is three pictures wide and is where
a room bigger than its picture is proved to be walkable end to end.
"""

import time

import pytest

from utils import McpClient, object_names


def inventory_names(state: dict) -> set:
    """The names of the things being carried."""
    return {item["name"] for item in state.get("inventory", [])}


pytestmark = [pytest.mark.xdist_group("maniac_deluxe")]

KIDS = ["bernard", "razor"]

#: Outside the mansion: where the intro leaves the team.
FRONT_GATE = 1

#: The front garden, which is three pictures wide and is entered at the street
#: end - the far end from the house.
FRONT_GARDEN = 2

#: The gate's own way into the garden: its left-hand edge.
GATE_TO_GARDEN = (5, 140)

#: What the picture shows of a room at once, in room coordinates.
SCREEN_WIDTH = 320

#: How near a walk has to land to count as having arrived.
NEAR_ENOUGH = 24

DOOR_MAT = "door_mat"
FRONT_DOOR = "front_door_v"
KEY = "key"

#: Inside: the hall the front door opens into, the left-hand of its three
#: doors, and the kitchen behind it.
HALL = 3
HALL_LEFT_DOOR = "door_v_2"
KITCHEN = 25

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


def test_07_maniac_deluxe_walks_the_length_of_a_scrolling_room(
    maniac_deluxe_client: McpClient,
) -> None:
    """A thing at the far end of a room three screens wide is still walked to.

    The front garden is 960 pixels of room behind a 320-pixel picture, and the
    team comes into it at the street end, with the door mat two screens away.
    Everything ``state`` names is named in room coordinates, and a click lands
    on the screen: aimed at the mat's own x of 336 while the camera stands at
    640, the click used to land on the right-hand edge of the picture - which
    is this room's way back out to the street, and so walking to the door mat
    walked out of the room instead. The walk is made in stages now, one per
    screenful, until the mat is in frame.
    """
    state = maniac_deluxe_client.state()
    assert state["room"]["id"] == FRONT_GATE, f"not at the gate: {state}"
    # Out of the gate and into the garden, which is entered at its far end.
    result = maniac_deluxe_client.walk(*GATE_TO_GARDEN)
    assert result["room"]["id"] == FRONT_GARDEN, f"never left the gate: {result}"

    state = maniac_deluxe_client.state()
    mat = next((o for o in state["objects"] if o["name"] == DOOR_MAT), None)
    assert mat is not None, f"no door mat in the garden: {object_names(state)}"
    assert state["position"]["x"] - mat["x"] > SCREEN_WIDTH, (
        f"the mat is meant to start off screen: {state['position']} vs {mat}"
    )

    result = maniac_deluxe_client.act("walk_to", DOOR_MAT)
    assert result["room"]["id"] == FRONT_GARDEN, (
        f"walking to the door mat left the garden: {result}"
    )
    assert abs(result["position"]["x"] - mat["x"]) <= NEAR_ENOUGH, (
        f"not at the door mat: {result['position']} vs {mat}"
    )


def test_08_maniac_deluxe_a_half_written_walk_to_point_is_not_used(
    maniac_deluxe_client: McpClient,
) -> None:
    """The front door is where the door is, not at the room's left-hand edge.

    Its hotspot carries a WalkTo point of (0, 64) - half a point, which is the
    editor's way of having none - and taking that literally sent the walk to
    the far edge of a room three screens wide. A hotspot needs both halves of
    the point before it is believed; otherwise its shape in the room's mask
    says where it is.
    """
    state = maniac_deluxe_client.state()
    door = next((o for o in state["objects"] if o["name"] == FRONT_DOOR), None)
    assert door is not None, f"no front door in the garden: {object_names(state)}"
    mat = next((o for o in state["objects"] if o["name"] == DOOR_MAT), None)
    assert mat is not None, f"no door mat in the garden: {object_names(state)}"
    assert abs(door["x"] - mat["x"]) <= SCREEN_WIDTH, (
        f"the door is not by its own mat: {door} vs {mat}"
    )


def test_09_maniac_deluxe_gets_into_the_mansion(
    maniac_deluxe_client: McpClient,
) -> None:
    """The opening of the game, played through: the key under the mat, the
    front door it unlocks, and the kitchen beyond the hall.

    Every verb the bar has is a button with the word painted into its sprite,
    so nothing on it is labelled and nothing says which one is chosen; `act`
    presses the one it wants before each action. This is the test that says
    those presses land on the verb they are named for - a run of the game with
    the wrong button under `pull` gets no key and goes no further.
    """
    state = maniac_deluxe_client.state()
    assert state["room"]["id"] == FRONT_GARDEN, f"not in the garden: {state}"

    maniac_deluxe_client.act("walk_to", DOOR_MAT)
    maniac_deluxe_client.act("pull", DOOR_MAT)
    names = object_names(maniac_deluxe_client.state())
    assert KEY in names, f"pulling the mat uncovered no key: {sorted(names)}"

    result = maniac_deluxe_client.act("pick_up", KEY)
    assert KEY in inventory_names(maniac_deluxe_client.state()), (
        f"the key was not picked up: {result}"
    )

    maniac_deluxe_client.act("use", FRONT_DOOR, KEY)
    result = maniac_deluxe_client.act("walk_to", FRONT_DOOR)
    assert result["room"]["id"] == HALL, f"the front door did not open: {result}"

    maniac_deluxe_client.act("open", HALL_LEFT_DOOR)
    result = maniac_deluxe_client.act("walk_to", HALL_LEFT_DOOR)
    assert result["room"]["id"] == KITCHEN, f"never reached the kitchen: {result}"
    names = object_names(maniac_deluxe_client.state())
    assert "refrigerator_v" in names, f"this is not the kitchen: {sorted(names)}"


def test_10_maniac_deluxe_switch_character_rejects_a_stranger(
    maniac_deluxe_client: McpClient,
) -> None:
    """A kid who was never picked is not on the team and cannot be switched to."""
    with pytest.raises(RuntimeError, match="nobody on the team"):
        maniac_deluxe_client.switch_character("jeff")
