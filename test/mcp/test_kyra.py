"""
Integration tests for the Kyrandia bridge (engine ``kyra``).

Kyrandia has a single mouse button. A click on an item lying in the room puts
it in the *hand*, not in the inventory: a carried item lives in one of the
boxes along the bottom of the screen, and a player gets it there with a second
click, on a box. The bridge replays exactly those clicks, so these tests check
what a player would see: the item ends up in a box, the hand ends up empty, and
the result says what was added and removed.

The first game's opening room also holds the case these tests exist for: the
first time Brandon tries to leave his house, the house itself talks to him for
over a minute. An action that sets that off must not time out, a caller that
gave up waiting must still be able to read every line from ``state``, and
``skip`` must be able to cut it short.

Each test starts from the save the engine writes itself when the intro ends
(slot 0), so none depends on another.
"""

import os
import subprocess
import sys
import time

import pytest

from mcp_client import McpClient
from state_helpers import wait_until_taking_input

KYRA_GAMES = ["kyra1", "kyra2", "kyra3"]


def _client(request, fixture: str) -> McpClient:
    return request.getfixturevalue(f"{fixture}_client")


def _wait_for_control(client: McpClient, budget: float = 180.0) -> tuple[dict, list]:
    """Poll state until the game takes input again; return it and every line read."""
    deadline = time.time() + budget
    lines: list = []
    while time.time() < deadline:
        state = client.state()
        lines += state.get("messages") or []
        if state.get("can_act"):
            return state, lines
        time.sleep(1.0)
    raise AssertionError(f"the game never gave control back; read {lines}")


def _act_and_give_up(client: McpClient, target: str, seconds: float) -> None:
    """Send act from a client that is killed before the action is over.

    A read timeout would never fire: the stream carries every line as it is
    said, so a client only gives up on a long action by giving up on the whole
    call, the way an agent harness's own tool timeout does.
    """
    script = (
        "import sys; sys.path.insert(0, sys.argv[1]);"
        "from mcp_client import wait_for_mcp;"
        "c = wait_for_mcp('127.0.0.1', int(sys.argv[2]), connect_timeout=30);"
        "c.set_timeout(600); c.act('use', sys.argv[3])"
    )
    proc = subprocess.Popen(
        [
            sys.executable,
            "-c",
            script,
            os.path.dirname(__file__),
            str(client.port),
            target,
        ]
    )
    try:
        proc.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    else:
        raise AssertionError(f"act on {target} was over in under {seconds}s")


@pytest.mark.parametrize("fixture", KYRA_GAMES)
def test_the_verbs_are_the_one_button(request, fixture: str) -> None:
    """No right button and no verb bar: 'use' and 'pick_up' are all there is."""
    state = wait_until_taking_input(_client(request, fixture), fixture)
    assert state.get("verbs") == ["use", "pick_up"], state.get("verbs")


@pytest.mark.parametrize("fixture", KYRA_GAMES)
def test_a_verb_the_game_does_not_have_is_refused_with_the_ones_it_does(
    request, fixture: str
) -> None:
    client = _client(request, fixture)
    state = wait_until_taking_input(client, fixture, want_objects=True)
    target = state["objects"][0]["name"]
    with pytest.raises(RuntimeError) as caught:
        client.act("look_at", target)
    assert "'use'" in str(caught.value) and "'pick_up'" in str(caught.value)


@pytest.mark.parametrize("fixture", KYRA_GAMES)
def test_the_ways_out_are_pathways(request, fixture: str) -> None:
    state = wait_until_taking_input(_client(request, fixture), fixture)
    exits = [o for o in state.get("objects") or [] if o.get("kind") == "exit"]
    assert exits, f"{fixture}: no way out of the first room: {state.get('objects')}"
    assert all(o.get("pathway") is True for o in exits), exits


def test_kyra1_take_puts_the_item_in_a_box(kyra1_client: McpClient) -> None:
    """The garnet on the table goes into a box, not into the hand."""
    state = wait_until_taking_input(kyra1_client, "kyra1", want_objects=True)
    assert "garnet" in [o["name"] for o in state["objects"]], state["objects"]
    boxes_before = state["free_boxes"]

    result = kyra1_client.act("pick_up", "garnet")

    assert result.get("inventory_added") == ["garnet"], result
    assert not result.get("inventory_removed"), result
    assert "held_item" not in result, result
    after = kyra1_client.state()
    carried = {i["name"]: i for i in after["inventory"]}
    assert "garnet" in carried and not carried["garnet"].get("held"), after
    assert after["free_boxes"] == boxes_before - 1, after
    assert "garnet" not in [o["name"] for o in after["objects"]], after
    debug = kyra1_client.call_tool("debug", {})
    assert debug["item_in_hand"] == -1, debug


def test_kyra1_a_carried_item_can_be_used_on_the_room(kyra1_client: McpClient) -> None:
    """Out of its box, onto a spot of the room, and back into a box."""
    wait_until_taking_input(kyra1_client, "kyra1", want_objects=True)
    kyra1_client.act("pick_up", "garnet")

    # The cauldron, by the window: a script-only hotspot with no name.
    result = kyra1_client.act("use", "garnet", x=45, y=100)

    assert "error" not in result, result
    after = kyra1_client.state()
    assert "held_item" not in after, after
    # Either it went back in its box, or the game kept it; never in the hand.
    assert all(not i.get("held") for i in after["inventory"]), after


def test_kyra1_a_carried_item_can_be_used_on_another(kyra1_client: McpClient) -> None:
    """Item on item is a click on the other item's box."""
    wait_until_taking_input(kyra1_client, "kyra1", want_objects=True)
    kyra1_client.act("pick_up", "garnet")
    with pytest.raises(RuntimeError) as caught:
        kyra1_client.act("use", "garnet", "garnet")
    assert "itself" in str(caught.value)


def test_kyra1_the_long_speech_does_not_time_out(kyra1_client: McpClient) -> None:
    """Trying the door the first time starts the house talking; the call lasts."""
    wait_until_taking_input(kyra1_client, "kyra1")
    kyra1_client.set_timeout(300.0)

    result = kyra1_client.act("use", "exit_south")

    lines = [m.get("text") for m in result.get("messages") or []]
    assert len(lines) >= 3, result
    assert result.get("can_act") is True, result


def test_kyra1_a_caller_that_gave_up_reads_the_rest_from_state(
    kyra1_client: McpClient,
) -> None:
    """The client times out mid-speech; nothing said is lost."""
    wait_until_taking_input(kyra1_client, "kyra1")
    _act_and_give_up(kyra1_client, "exit_south", 15.0)
    time.sleep(1.0)

    first = kyra1_client.state()
    assert first.get("can_act") is False, first
    assert first.get("action_in_progress") is True, first
    _, lines = _wait_for_control(kyra1_client)
    lines = (first.get("messages") or []) + lines
    texts = [m.get("text") for m in lines]
    assert len(texts) >= 3, texts
    assert any(m.get("type") == "actor" for m in lines), lines


def test_kyra1_the_speech_can_be_skipped(kyra1_client: McpClient) -> None:
    wait_until_taking_input(kyra1_client, "kyra1")
    _act_and_give_up(kyra1_client, "exit_south", 10.0)
    kyra1_client.set_timeout(120.0)
    # Read first, the way a caller coming back does. A call landing in the
    # very frame the server notices the old client gone is still queued behind
    # that stream and answered with an empty 202.
    time.sleep(1.0)
    assert kyra1_client.state().get("can_act") is False

    started = time.time()
    result = kyra1_client.skip()

    assert result.get("can_act") is True, result
    assert time.time() - started < 40.0, "the skip took as long as the speech"


def test_kyra1_through_several_rooms(kyra1_client: McpClient) -> None:
    """Out of the house and on to the next rooms, by the exits state names."""
    wait_until_taking_input(kyra1_client, "kyra1")
    kyra1_client.set_timeout(300.0)
    visited = [kyra1_client.state()["room"]["id"]]
    for _ in range(6):
        state, _ = _wait_for_control(kyra1_client)
        exits = [o["name"] for o in state["objects"] if o.get("pathway")]
        assert exits, state
        result = kyra1_client.act("use", exits[0])
        room = result.get("room_changed")
        if room is not None:
            visited.append(room)
        if len(set(visited)) >= 3:
            break
    assert len(set(visited)) >= 3, visited
