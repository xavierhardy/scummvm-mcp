"""
Integration tests for the full Maniac Mansion (V1/DOS) title screen.

The full game opens on the title screen, where the three heroes are picked
before anything else happens — there is no save to load past it, so the whole
sequence runs in order on one session-scoped instance (like the atlantis/ft
demos) and is pinned to a single xdist worker.

Walkthrough: the title screen advertises itself through state's
kid_selection_pending flag -> choose_kids names the two kids joining Dave, which
clicks their portraits, presses START and escapes through the intro -> the game
is running with the chosen team on screen and the selection is gone.
"""

import pytest

from utils import McpClient, object_names

pytestmark = [pytest.mark.xdist_group("maniac_full")]

KIDS = ["bernard", "razor"]


def test_01_maniac_full_title_screen(maniac_full_client: McpClient) -> None:
    """The title screen is flagged as waiting for the kid selection."""
    state = maniac_full_client.state()
    assert state.get("kid_selection_pending") is True, (
        f"expected the title screen's kid selection, got {state}"
    )
    # Nothing is playable yet: no verb bar, and the portraits carry no names.
    assert not state.get("verbs"), f"unexpected verbs on the title screen: {state}"


def test_02_maniac_full_choose_kids_starts_game(maniac_full_client: McpClient) -> None:
    """choose_kids picks the team by name and leaves the player in control."""
    result = maniac_full_client.choose_kids(KIDS)
    assert result.get("kids") == KIDS, f"unexpected team: {result}"

    state = maniac_full_client.state()
    assert "kid_selection_pending" not in state, (
        f"title screen still up after choose_kids: {state}"
    )
    # The game proper: the verb bar is up and the two chosen kids stand with Dave.
    assert "walk to" in state.get("verbs", []), f"no verb bar: {state}"
    names = object_names(state)
    for kid in KIDS:
        assert kid in names, f"{kid} is not in the room: {sorted(names)}"


def test_03_maniac_full_choose_kids_rejected_in_game(
    maniac_full_client: McpClient,
) -> None:
    """Once the game has started there is no selection left to make."""
    with pytest.raises(RuntimeError, match="not on screen"):
        maniac_full_client.choose_kids(["wendy", "jeff"])
