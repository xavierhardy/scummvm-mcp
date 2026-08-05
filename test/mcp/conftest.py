"""Pytest fixtures for MCP integration tests.

The suite runs under ``--dist=loadgroup`` (see pytest.ini): tests are
distributed per-test across xdist workers, so most game fixtures are
**function-scoped** — each test launches its own fresh ScummVM instance from its
save slot, runs, and tears it down. A fresh process makes every test independent
of its siblings, which is what lets them run in parallel.

Full Throttle and Atlantis demos cannot save/load arbitrary states, so their
tests are strictly-ordered single-instance walkthroughs. Those two fixtures are
**session-scoped** and their tests are pinned to one worker via
``xdist_group`` marks (``test_ft.py`` / ``test_atlantis.py``).

Ports are assigned per (worker, fixture) by :func:`utils.get_mcp_port` so the
many concurrent instances never collide.
"""

import os
from collections.abc import Iterator
from typing import TextIO

import pytest

from utils import (
    GAME_PATHS,
    MCP_CONNECT_TIMEOUT_SECS,
    MCP_HOST,
    McpClient,
    get_mcp_port,
    launch_scummvm,
    require_game_path,
    require_save_slot,
    wait_for_mcp,
)

PROC_KILL_TIMEOUT_SECS = 5

_SKIP_SLOW = os.environ.get("SKIP_SLOW_TESTS", "").lower() in ("1", "true")


def pytest_collection_modifyitems(config, items):
    """Skip tests marked ``slow`` when SKIP_SLOW_TESTS is true/1."""
    if not _SKIP_SLOW:
        return
    skip_slow = pytest.mark.skip(reason="SKIP_SLOW_TESTS is set")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip_slow)


# Stable per-fixture index (0..99) fed to get_mcp_port so each fixture gets a
# distinct port within its worker's band. Never reorder/reuse these.
_FIXTURE_INDEX = {
    "monkey": 0,
    "monkey_de": 1,
    "maniac": 2,
    "maniac_phone": 3,
    "atlantis": 4,
    "samnmax": 5,
    "samnmax_street": 6,
    "ft": 7,
    "indy3": 8,
    "indy3_travel": 9,
    "loom": 10,
    "loom_leaf": 11,
    "dig": 12,
    "dig_wreck": 13,
    "comi_s3": 14,
    "comi_s4": 15,
    "comi": 16,
    "monkey_bar": 17,
    "monkey_kitchen": 18,
    "monkey_prison": 19,
    "sword1": 20,
    "sky": 21,
    "queen": 22,
    "woodruff": 23,
    "zak": 24,
    "tentacle": 25,
    "monkey2": 26,
    "maniac_full": 27,
}


def _client(
    game_id: str, fixture_key: str, save_slot: int = 1, checkpoint: bool = False
) -> Iterator[McpClient]:
    """Launch ScummVM for *game_id*, yield a connected McpClient, then tear down.

    Shared by every fixture. The port is unique per (worker, fixture); the
    launcher copies the save slots into a private temp dir (isolate_saves), so
    concurrent instances of the same game never share save files. ``checkpoint``
    fixtures additionally skip when their slot has not been captured yet.
    """
    require_game_path(game_id)
    if checkpoint:
        require_save_slot(game_id, save_slot)
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    port = get_mcp_port(_FIXTURE_INDEX[fixture_key])
    proc = launch_scummvm(
        game_id,
        GAME_PATHS[game_id],
        port=port,
        scummvm_binary=scummvm_binary,
        save_slot=save_slot,
    )
    client = wait_for_mcp(MCP_HOST, port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    try:
        yield client
    finally:
        client.close()
        proc.kill()
        proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
        # Close log file handles stashed on the process by launch_scummvm.
        handles: tuple[TextIO, TextIO] | None = getattr(proc, "_log_handles", None)
        if handles is not None:
            for handle in handles:
                handle.close()


# ---------------------------------------------------------------------------
# Function-scoped fixtures — a fresh instance per test (parallel, per-test).
# ---------------------------------------------------------------------------


@pytest.fixture
def monkey_client() -> Iterator[McpClient]:
    """Monkey Island 1 EGA demo (office room, slot 1)."""
    yield from _client("monkey-ega-demo", "monkey")


@pytest.fixture
def monkey_de_client() -> Iterator[McpClient]:
    """German Monkey Island 1 EGA demo (slot 1)."""
    yield from _client("monkey-ega-demo-de", "monkey_de")


# Monkey Island 1 (English) is a deep item-collection walkthrough. To keep its
# tests per-test independent, deeper stages load checkpoint save states captured
# by make_save_states.py (slots 6/7/8). Until those are captured on a machine
# with the demo data, these fixtures skip via require_save_slot.
@pytest.fixture
def monkey_bar_client() -> Iterator[McpClient]:
    """Monkey 1 EGA demo checkpoint: the SCUMM bar (room 52), bowl o' mints present."""
    yield from _client("monkey-ega-demo", "monkey_bar", save_slot=6, checkpoint=True)


@pytest.fixture
def monkey_kitchen_client() -> Iterator[McpClient]:
    """Monkey 1 EGA demo checkpoint: the kitchen (room 51), hunk o' meat present,
    breath mint already in inventory."""
    yield from _client(
        "monkey-ega-demo", "monkey_kitchen", save_slot=7, checkpoint=True
    )


@pytest.fixture
def monkey_prison_client() -> Iterator[McpClient]:
    """Monkey 1 EGA demo checkpoint: the prison (room 54) with the breath mint
    in inventory, ready to give to the prisoner."""
    yield from _client("monkey-ega-demo", "monkey_prison", save_slot=8, checkpoint=True)


@pytest.fixture
def maniac_client() -> Iterator[McpClient]:
    """Maniac Mansion C64 demo (slot 1, outside the mansion)."""
    yield from _client("maniac-c64", "maniac")


@pytest.fixture
def maniac_phone_client() -> Iterator[McpClient]:
    """Maniac Mansion C64 demo (slot 2, next to the phone)."""
    yield from _client("maniac-c64", "maniac_phone", save_slot=2)


@pytest.fixture
def samnmax_client() -> Iterator[McpClient]:
    """Sam & Max Hit the Road demo (slot 1, the office, room 7)."""
    yield from _client("samnmax", "samnmax")


@pytest.fixture
def samnmax_street_client() -> Iterator[McpClient]:
    """Sam & Max Hit the Road demo (slot 2, the street, room 9) with Max exposed
    as 'max_the_object' in the inventory for two-target interactions."""
    yield from _client("samnmax", "samnmax_street", save_slot=2)


@pytest.fixture
def indy3_client() -> Iterator[McpClient]:
    """Passport to Adventure, Indy3 segment (slot 3, the boxing gym)."""
    yield from _client("pass", "indy3", save_slot=3)


@pytest.fixture
def indy3_travel_client() -> Iterator[McpClient]:
    """Passport to Adventure, Indy3 segment (slot 4, the Pan Am clipper, room 24)."""
    yield from _client("pass", "indy3_travel", save_slot=4)


@pytest.fixture
def loom_client() -> Iterator[McpClient]:
    """Passport to Adventure, Loom segment (slot 1)."""
    yield from _client("pass", "loom")


@pytest.fixture
def loom_leaf_client() -> Iterator[McpClient]:
    """Passport to Adventure, Loom segment (slot 2, the leaf/pathway scene, room 36)."""
    yield from _client("pass", "loom_leaf", save_slot=2)


@pytest.fixture
def dig_client() -> Iterator[McpClient]:
    """The Dig demo (slot 1, canyon room 15 with Brink and Maggie)."""
    yield from _client("dig-demo", "dig")


@pytest.fixture
def dig_wreck_client() -> Iterator[McpClient]:
    """The Dig demo (slot 5, the wreck interior, room 19)."""
    yield from _client("dig-demo", "dig_wreck", save_slot=5)


@pytest.fixture
def comi_client() -> Iterator[McpClient]:
    """Curse of Monkey Island demo (slot 1, the cannon beach)."""
    yield from _client("comi-demo", "comi")


@pytest.fixture
def comi_s3_client() -> Iterator[McpClient]:
    """Curse of Monkey Island demo (slot 3, ramrod + plastic hook in inventory)."""
    yield from _client("comi-demo", "comi_s3", save_slot=3)


@pytest.fixture
def comi_s4_client() -> Iterator[McpClient]:
    """Curse of Monkey Island demo (slot 4, the cannon minigame)."""
    yield from _client("comi-demo", "comi_s4", save_slot=4)


@pytest.fixture
def sword1_client() -> Iterator[McpClient]:
    """Broken Sword 1 demo (slot 1: George on rue Jarry, outside the café)."""
    yield from _client("sword1-demo", "sword1", save_slot=1, checkpoint=True)


@pytest.fixture
def sky_client() -> Iterator[McpClient]:
    """Beneath a Steel Sky (slot 1: Foster on screen 0, top of the walkway,
    right after the intro)."""
    yield from _client("sky", "sky", save_slot=1, checkpoint=True)


@pytest.fixture
def queen_client() -> Iterator[McpClient]:
    """Flight of the Amazon Queen (slot 1: Joe locked in the hotel room,
    right after the intro)."""
    yield from _client("queen", "queen", save_slot=1, checkpoint=True)


# The three full games below are checkpointed right after their intro (they are
# not demos, so there is no short scripted opening to replay per test).
@pytest.fixture
def zak_client() -> Iterator[McpClient]:
    """Zak McKracken (V2, slot 1: Zak's bedroom, room 1, right after the intro)."""
    yield from _client("zak", "zak", save_slot=1, checkpoint=True)


@pytest.fixture
def tentacle_client() -> Iterator[McpClient]:
    """Day of the Tentacle (V6, slot 1: Bernard in the mansion lobby, room 34)."""
    yield from _client("tentacle", "tentacle", save_slot=1, checkpoint=True)


@pytest.fixture
def monkey2_client() -> Iterator[McpClient]:
    """Monkey Island 2 (V5, slot 1: Guybrush on the Scabb Island dock, room 7,
    right after the intro — walking on triggers Largo's toll-bridge dialog)."""
    yield from _client("monkey2", "monkey2", save_slot=1, checkpoint=True)


# ---------------------------------------------------------------------------
# Session-scoped fixtures — no-save games whose tests must run as one ordered
# sequence on a single instance (pinned to one worker via xdist_group marks).
# ---------------------------------------------------------------------------


@pytest.fixture(scope="session")
def atlantis_client() -> Iterator[McpClient]:
    """Indiana Jones: Fate of Atlantis demo (no save support; intro-driven).

    The demo's boot script overrides the configured talkspeed (it sets
    VAR_CHARINC outside room 0, so the engine's room-0-only user override is
    skipped), so force the max text speed at runtime via the mcp_debug-gated
    set_talk_speed tool.
    """
    for client in _client("atlantis", "atlantis"):
        client.set_talk_speed(255)
        yield client


@pytest.fixture(scope="session")
def maniac_full_client() -> Iterator[McpClient]:
    """Maniac Mansion, the full game (V1/DOS).

    Started fresh at the title screen, where the three heroes are still to be
    picked, so there is nothing to load: the tests run as one ordered sequence
    on a single instance (like the atlantis/ft demos)."""
    yield from _client("maniac", "maniac_full")


@pytest.fixture(scope="session")
def ft_client() -> Iterator[McpClient]:
    """Full Throttle demo (no save support; ordered storyline walkthrough)."""
    yield from _client("ft-demo", "ft")


@pytest.fixture(scope="session")
def woodruff_client() -> Iterator[McpClient]:
    """The Bizarre Adventures of Woodruff and the Schnibble (Gob engine).

    No save support: the whole run is one ordered sequence on a single instance,
    started fresh and skipped past the intro (like the atlantis/ft demos)."""
    yield from _client("woodruff", "woodruff")
