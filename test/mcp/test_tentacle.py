"""
Integration test for Day of the Tentacle (SCUMM V6, floppy DOS).

DOTT is a V6 game that kept the classic V3-V5 *text* verb bar, so its bridge
(``McpBridgeTentacle``) opts out of the V6 icon-verb/icon-dialog heuristics.
Without that, the verb bar reads as a permanently-pending dialog question and
state.verbs comes back empty — the first two tests below pin that down.

It is a full game, so the rest are compatibility smoke tests from the committed
slot-1 save (Bernard in the mansion lobby, room 34, right after the intro).
"""

from assertions import (
    assert_has_position,
    assert_inventory_contains,
    assert_message_contains,
    assert_room,
)
from utils import McpClient, bind_verb, find_id

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


def test_tentacle_initial_state(tentacle_client: McpClient) -> None:
    """The save lands in the mansion lobby with the full verb bar and its objects."""
    state = tentacle_client.state()
    assert_room(state, 34)

    assert sorted(state["verbs"]) == sorted(EXPECTED_VERBS), (
        f"unexpected DOTT verb bar: {state['verbs']}"
    )

    names = {obj["name"] for obj in state["objects"]}
    for expected in ("dime", "pay_phone", "grandfather_clock", "chuck_the_plant"):
        assert expected in names, (
            f"expected '{expected}' in room 34, got {sorted(names)}"
        )

    inventory = state["inventory"]
    assert inventory == ["textbook"], f"unexpected starting inventory: {inventory}"


def test_tentacle_verb_bar_is_not_a_dialog(tentacle_client: McpClient) -> None:
    """Normal play reports verbs, not a pending question, and no glyph garbage.

    DOTT spells the double 'o' of "Look at" with a ligature from its own font,
    which decodes to a stray high-byte character; the bridge rewrites the bar to
    canonical names so agents can echo the labels back.
    """
    state = tentacle_client.state()

    assert state.get("question") is None, (
        f"the verb bar must not read as a dialog: {state.get('question')}"
    )
    assert "look at" in state["verbs"], f"missing 'look at': {state['verbs']}"
    for verb in state["verbs"]:
        assert verb.isascii(), f"verb label carries a game-font glyph: {verb!r}"


def test_tentacle_look_at_help_wanted_sign(tentacle_client: McpClient) -> None:
    """'look at' reaches the object script and Bernard reads the sign out."""
    look_at = bind_verb(tentacle_client, "look_at")

    result = look_at("help_wanted_sign")
    assert_message_contains(result, "HELP WANTED")
    assert_has_position(result)


def test_tentacle_pick_up_dime(tentacle_client: McpClient) -> None:
    """The dime in the pay phone's coin return goes into the inventory."""
    pick_up = bind_verb(tentacle_client, "pick_up")

    result = pick_up("dime")
    assert_inventory_contains(result, "dime")


def test_tentacle_open_grandfather_clock(tentacle_client: McpClient) -> None:
    """Opening the grandfather clock walks Bernard into the passage behind it."""
    open_ = bind_verb(tentacle_client, "open")

    result = open_("grandfather_clock")
    room_changed = result.get("room_changed")
    assert room_changed == 31, f"expected the clock passage (room 31), got {result}"


def test_tentacle_stairs_are_a_pathway(tentacle_client: McpClient) -> None:
    """The stairs are exposed as a walk-only exit and answer when walked to."""
    state = tentacle_client.state()
    stairs = find_id(state, "stairs")
    assert stairs is not None, "no 'stairs' object in room 34"
    stairs_obj = next(obj for obj in state["objects"] if obj["id"] == stairs)
    assert stairs_obj["pathway"], f"stairs should be a pathway: {stairs_obj}"

    result = bind_verb(tentacle_client, "walk_to")(stairs)
    assert_message_contains(result, "Hoagie")
