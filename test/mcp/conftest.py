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
}


def _client(
    game_id: str, fixture_key: str, save_slot: int = 1, checkpoint: bool = False
):
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
        # Close log file handles opened by launch_scummvm.
        if hasattr(proc, "_stdout_file"):
            proc._stdout_file.close()
        if hasattr(proc, "_stderr_file"):
            proc._stderr_file.close()


# ---------------------------------------------------------------------------
# Function-scoped fixtures — a fresh instance per test (parallel, per-test).
# ---------------------------------------------------------------------------


@pytest.fixture
def monkey_client() -> McpClient:
    """Monkey Island 1 EGA demo (office room, slot 1)."""
    yield from _client("monkey-ega-demo", "monkey")


@pytest.fixture
def monkey_de_client() -> McpClient:
    """German Monkey Island 1 EGA demo (slot 1)."""
    yield from _client("monkey-ega-demo-de", "monkey_de")


# Monkey Island 1 (English) is a deep item-collection walkthrough. To keep its
# tests per-test independent, deeper stages load checkpoint save states captured
# by make_save_states.py (slots 6/7/8). Until those are captured on a machine
# with the demo data, these fixtures skip via require_save_slot.
@pytest.fixture
def monkey_bar_client() -> McpClient:
    """Monkey 1 EGA demo checkpoint: the SCUMM bar (room 52), bowl o' mints present."""
    yield from _client("monkey-ega-demo", "monkey_bar", save_slot=6, checkpoint=True)


@pytest.fixture
def monkey_kitchen_client() -> McpClient:
    """Monkey 1 EGA demo checkpoint: the kitchen (room 51), hunk o' meat present,
    breath mint already in inventory."""
    yield from _client(
        "monkey-ega-demo", "monkey_kitchen", save_slot=7, checkpoint=True
    )


@pytest.fixture
def monkey_prison_client() -> McpClient:
    """Monkey 1 EGA demo checkpoint: the prison (room 54) with the breath mint
    in inventory, ready to give to the prisoner."""
    yield from _client("monkey-ega-demo", "monkey_prison", save_slot=8, checkpoint=True)


@pytest.fixture
def maniac_client() -> McpClient:
    """Maniac Mansion C64 demo (slot 1, outside the mansion)."""
    yield from _client("maniac-c64", "maniac")


@pytest.fixture
def maniac_phone_client() -> McpClient:
    """Maniac Mansion C64 demo (slot 2, next to the phone)."""
    yield from _client("maniac-c64", "maniac_phone", save_slot=2)


@pytest.fixture
def samnmax_client() -> McpClient:
    """Sam & Max Hit the Road demo (slot 1, the office, room 7)."""
    yield from _client("samnmax", "samnmax")


@pytest.fixture
def samnmax_street_client() -> McpClient:
    """Sam & Max Hit the Road demo (slot 2, the street, room 9) with Max exposed
    as 'max_the_object' in the inventory for two-target interactions."""
    yield from _client("samnmax", "samnmax_street", save_slot=2)


@pytest.fixture
def indy3_client() -> McpClient:
    """Passport to Adventure, Indy3 segment (slot 3, the boxing gym)."""
    yield from _client("pass", "indy3", save_slot=3)


@pytest.fixture
def indy3_travel_client() -> McpClient:
    """Passport to Adventure, Indy3 segment (slot 4, the Pan Am clipper, room 24)."""
    yield from _client("pass", "indy3_travel", save_slot=4)


@pytest.fixture
def loom_client() -> McpClient:
    """Passport to Adventure, Loom segment (slot 1)."""
    yield from _client("pass", "loom")


@pytest.fixture
def loom_leaf_client() -> McpClient:
    """Passport to Adventure, Loom segment (slot 2, the leaf/pathway scene, room 36)."""
    yield from _client("pass", "loom_leaf", save_slot=2)


@pytest.fixture
def dig_client() -> McpClient:
    """The Dig demo (slot 1, canyon room 15 with Brink and Maggie)."""
    yield from _client("dig-demo", "dig")


@pytest.fixture
def dig_wreck_client() -> McpClient:
    """The Dig demo (slot 5, the wreck interior, room 19)."""
    yield from _client("dig-demo", "dig_wreck", save_slot=5)


@pytest.fixture
def comi_client() -> McpClient:
    """Curse of Monkey Island demo (slot 1, the cannon beach)."""
    yield from _client("comi-demo", "comi")


@pytest.fixture
def comi_s3_client() -> McpClient:
    """Curse of Monkey Island demo (slot 3, ramrod + plastic hook in inventory)."""
    yield from _client("comi-demo", "comi_s3", save_slot=3)


@pytest.fixture
def comi_s4_client() -> McpClient:
    """Curse of Monkey Island demo (slot 4, the cannon minigame)."""
    yield from _client("comi-demo", "comi_s4", save_slot=4)


# ---------------------------------------------------------------------------
# Session-scoped fixtures — no-save games whose tests must run as one ordered
# sequence on a single instance (pinned to one worker via xdist_group marks).
# ---------------------------------------------------------------------------


@pytest.fixture(scope="session")
def atlantis_client() -> McpClient:
    """Indiana Jones: Fate of Atlantis demo (no save support; intro-driven)."""
    yield from _client("atlantis", "atlantis")


@pytest.fixture(scope="session")
def ft_client() -> McpClient:
    """Full Throttle demo (no save support; ordered storyline walkthrough)."""
    yield from _client("ft-demo", "ft")
