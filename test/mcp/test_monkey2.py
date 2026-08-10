"""
Integration test for Monkey Island 2: LeChuck's Revenge (SCUMM V5, DOS).

MI2 is a full game, so these are compatibility smoke tests rather than a
walkthrough: every test starts from the committed slot-1 save (Guybrush on the
Scabb Island dock, room 7, right after the intro). Walking west across the
bridge is what triggers Largo's toll-bridge conversation, which the dialog
tests use.
"""

from assertions import (
    assert_has_position,
    assert_inventory_contains,
    assert_message_contains,
    assert_room,
)
from utils import McpClient, bind_verb, choice_labels

EXPECTED_VERBS = [
    "give",
    "open",
    "close",
    "pick up",
    "look at",
    "talk to",
    "use",
    "push",
    "pull",
    "walk to",
]

# Three objects in room 7 are named "sign"; address them by id.
WOODTICK_SIGN = 46  # the "Welcome to Woodtick" sign next to Guybrush
BRIDGE_SIGN = 58  # across the bridge — walking there runs into Largo


def _provoke_largo(client: McpClient) -> dict:
    """Send Guybrush west across the bridge; Largo stops him. Returns the act."""
    return client.act("look_at", BRIDGE_SIGN)


def test_monkey2_initial_state(monkey2_client: McpClient) -> None:
    """The save lands on the Scabb Island dock with the full verb bar."""
    state = monkey2_client.state()
    assert_room(state, 7)

    assert sorted(state["verbs"]) == sorted(EXPECTED_VERBS), (
        f"unexpected MI2 verb bar: {state['verbs']}"
    )

    names = {obj["name"] for obj in state["objects"]}
    for expected in ("sign", "ship", "door"):
        assert expected in names, (
            f"expected '{expected}' in room 7, got {sorted(names)}"
        )

    inventory = state["inventory"]
    assert inventory, "expected Guybrush to be carrying his riches"
    assert set(inventory) == {"riches"}, f"unexpected starting inventory: {inventory}"


def test_monkey2_look_at_sign(monkey2_client: McpClient) -> None:
    """'look at' reaches the object script and Guybrush reads the sign out."""
    look_at = bind_verb(monkey2_client, "look_at")

    result = look_at(WOODTICK_SIGN)
    assert_message_contains(result, "Woodtick")
    assert_has_position(result)


def test_monkey2_pick_up_sign(monkey2_client: McpClient) -> None:
    """Pulling the Woodtick sign out of the ground yields the shovel."""
    pick_up = bind_verb(monkey2_client, "pick_up")

    result = pick_up(WOODTICK_SIGN)
    assert_inventory_contains(result, "shovel")


def test_monkey2_largo_dialog(monkey2_client: McpClient) -> None:
    """Largo's toll-bridge conversation is reported as a dialog question."""
    result = _provoke_largo(monkey2_client)

    question = result.get("question")
    assert question is not None, f"expected Largo's dialog, got {result}"
    labels = choice_labels(question)
    assert len(labels) == 4, f"expected 4 toll-bridge choices, got {labels}"
    assert any("bribe" in label for label in labels), f"unexpected choices: {labels}"
    assert_message_contains(result, "toll bridge")

    # The verb bar is swapped out for the choices while the dialog is up.
    state = monkey2_client.state()
    assert state["verbs"] == [], f"expected no verbs during a dialog: {state['verbs']}"
    assert state.get("question") is not None, "state should still report the dialog"


def test_monkey2_answer_largo(monkey2_client: McpClient) -> None:
    """Answering the first choice plays the line and opens the follow-up dialog."""
    _provoke_largo(monkey2_client)

    result = monkey2_client.answer(1)
    assert_message_contains(result, "bribe")
    assert_message_contains(result, "extortion")
    follow_up = result.get("question")
    assert follow_up is not None, f"expected a follow-up dialog, got {result}"
    assert choice_labels(follow_up), "follow-up dialog has no choices"


def test_monkey2_walk_moves_ego(monkey2_client: McpClient) -> None:
    """walk() moves Guybrush along the dock."""
    start = monkey2_client.state()["position"]

    result = monkey2_client.walk(start["x"] - 40, start["y"])
    assert_has_position(result)
    end = monkey2_client.state()["position"]
    assert end != start, f"walk did not move Guybrush (still at {end})"


# --- The click-only screens: the island map and the swamp coffin -------------
# Slot 2 stands in the swamp next to the coffin. Leaving by the path puts
# Guybrush on the Scabb Island map; climbing into the coffin turns the swamp
# into a rowing screen. On both, MI2 saves its verb bar away and the only
# interaction left is a click.

SWAMP = 20
ISLAND_MAP = 2
SWAMP_PATH = 202  # the way out of the swamp, back onto the map


def test_monkey2_map_travel_switches_room(monkey2_swamp_client: McpClient) -> None:
    """On the island map, walking to a place travels there."""
    monkey2_swamp_client.act("walk to", SWAMP_PATH)
    state = monkey2_swamp_client.state()
    assert_room(state, ISLAND_MAP)
    assert state["verbs"] == ["walk to"], f"unexpected map verbs: {state['verbs']}"
    assert state.get("question") is None, f"the map is not a dialog: {state}"

    places = {obj["name"]: obj for obj in state["objects"]}
    assert "woodtick" in places and "swamp" in places, f"map places: {sorted(places)}"
    assert places["swamp"]["compatible_verbs"] == ["walk to"], (
        f"map places must advertise the click: {places['swamp']}"
    )

    result = monkey2_swamp_client.act("walk to", "swamp")
    assert result.get("room_changed") == SWAMP, f"did not sail to the swamp: {result}"


def test_monkey2_coffin_rows_instead_of_walking(
    monkey2_swamp_client: McpClient,
) -> None:
    """In the coffin the verb bar becomes the game's own 'row to', and walk rows."""
    monkey2_swamp_client.act("use", "coffin")
    state = monkey2_swamp_client.state()
    assert_room(state, SWAMP)
    assert state["verbs"] == ["row to"], (
        f"the coffin should offer the game's rowing verb: {state['verbs']}"
    )
    # The bar being saved away used to read as a pending dialog, which locked
    # act() and walk() out of the whole swamp.
    assert state.get("question") is None, f"rowing is not a dialog: {state}"

    start = state["position"]
    result = monkey2_swamp_client.walk(start["x"] + 120, start["y"])
    assert_has_position(result)
    end = monkey2_swamp_client.state()["position"]
    assert end["x"] > start["x"], f"the coffin did not row east: {start} -> {end}"

    # Rowing to a named place works the same way.
    monkey2_swamp_client.act("row to", "shack")
    assert monkey2_swamp_client.state()["position"] != end, "rowing to the shack"
