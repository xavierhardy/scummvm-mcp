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
    has_captured_save,
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
    "zak_tv": 28,
    "monkey2_swamp": 29,
    # 30 is taken by test_session.py, which allocates its own port outside
    # these fixtures — do not reuse it here.
    "gob1": 31,
    "plain_tools": 32,
    "dw1": 33,
    "dw2": 34,
    "sword2": 35,
    "toon": 36,
    "gk1": 37,
    "sq6": 38,
    "gob2": 39,
    "gob3": 40,
    "ween": 41,
    "zak_repixeled": 42,
    "zak_seamonster": 43,
    "cstime": 44,
    # The full games. New indices only - never reuse one above.
    "ft_full": 45,
    "loom_full": 46,
    "indy3_full": 47,
    "atlantis_full": 48,
    "samnmax_full": 49,
    "monkey_full": 50,
    "dw_full": 51,
    "dw2_full": 52,
    "sword1_full": 53,
    "gob2_full": 54,
    "gob3_full": 55,
    "ween_full": 56,
    "gk1_full": 57,
    "sq6_full": 58,
    "kq5_full": 59,
    "kq6_full": 60,
    "kq7_full": 61,
    "sq4_full": 62,
    "sq5_full": 63,
    "pq3_full": 64,
    "kq1sci_full": 65,
    "kq4sci_full": 66,
    "sq1sci_full": 67,
    "qfg1_full": 68,
    "qfg2_full": 69,
    "sq2vga": 70,
    "pq2_full": 71,
    # The games whose engines had no bridge before.
    "kq2": 72,
    "kq3": 73,
    "pq1": 74,
    "kyra1": 75,
    "kyra2": 76,
    "kyra3": 77,
    "simon1": 78,
    "sanitarium": 79,
}


#: How long to keep asking an engine that binds its port long before it runs
#: its first game cycle. SCI loads its resources, sets up its sound driver and
#: runs its start-up script before it ever hands time back, and only a game
#: cycle answers a tool call. The number went up when AGS and Mohawk were
#: built in: every engine's detection table is walked at start-up, and those
#: two brought thousands of entries each.
SLOW_BOOT_SECS = 360.0

#: Longer still, for an engine whose detection table has to be walked before
#: the game starts. Mohawk's covers every Myst and Riven variant there is, and
#: on a slow machine that scan takes several minutes - during which the port is
#: bound but nothing answers, because only a game loop answers.
HUGE_TABLE_BOOT_SECS = 600.0


def _client(
    game_id: str,
    fixture_key: str,
    save_slot: int = 1,
    checkpoint: bool = False,
    ini_overrides: dict[str, str] | None = None,
    connect_timeout: float = MCP_CONNECT_TIMEOUT_SECS,
    request_timeout: float = MCP_CONNECT_TIMEOUT_SECS,
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
        ini_overrides=ini_overrides,
    )
    # connect_timeout is how long to keep asking; timeout is how long one
    # request may take. They were the same number until an engine turned up
    # that binds its port long before it runs its first game cycle - which is
    # what actually answers - and SCI takes about a minute to get there.
    # Connect with the ordinary per-request timeout: while the game is still
    # starting up every attempt fails at once, and a long one here would spend
    # the whole connect budget on a handful of tries. The longer timeout is
    # for the calls made afterwards, where a single action can genuinely take
    # a while.
    client = wait_for_mcp(
        MCP_HOST, port, connect_timeout=connect_timeout,
        timeout=MCP_CONNECT_TIMEOUT_SECS,
    )
    if request_timeout != MCP_CONNECT_TIMEOUT_SECS:
        client.set_timeout(request_timeout)
    # Where this instance's screenshot tool writes, for tests that look there.
    client.screenshot_path = getattr(proc, "screenshot_path", None)
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
def monkey2_swamp_client() -> Iterator[McpClient]:
    """Monkey Island 2 (slot 2: the swamp, standing next to the coffin).

    Both of MI2's click-only screens are one step away: the path leads back to
    the Scabb Island map, the coffin turns the swamp into a rowing screen.
    """
    yield from _client("monkey2", "monkey2_swamp", save_slot=2, checkpoint=True)


@pytest.fixture
def zak_tv_client() -> Iterator[McpClient]:
    """Zak McKracken (V2, slot 2: the living room with the TV playing).

    The TV prints a line every few seconds with the player in full control —
    the scene that used to hold every action's stream open until it timed out.
    """
    yield from _client("zak", "zak_tv", save_slot=2, checkpoint=True)


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


@pytest.fixture
def plain_tools_client() -> Iterator[McpClient]:
    """A game with every optional MCP setting turned off.

    The committed inis enable mcp_debug and mcp_skip_tool; this one does not,
    which is how the tool table is checked for what it must *not* carry."""
    yield from _client(
        "monkey-ega-demo",
        "plain_tools",
        ini_overrides={"mcp_debug": "false", "mcp_skip_tool": "false"},
    )


@pytest.fixture(scope="session")
def gk1_client() -> Iterator[McpClient]:
    """Gabriel Knight demo (SCI engine, SCI1.1, icon-bar verbs).

    No save support - the demo answers "game cannot be saved in the current
    state" wherever it is asked - so this is one ordered sequence on a single
    instance, clicked in from the title screen."""
    # A single act here can take a while: this game's verbs are cursors, and
    # reaching one means cycling the cursor with the right button until it is
    # the wanted one, several frames apart.
    yield from _client(
        "gk1-demo", "gk1", connect_timeout=SLOW_BOOT_SECS, request_timeout=120.0
    )


@pytest.fixture(scope="session")
def sq6_client() -> Iterator[McpClient]:
    """Space Quest 6 demo (SCI engine, SCI2.1/SCI32, icon-bar verbs).

    Same shape as gk1: no save support, one ordered sequence, skipped in."""
    yield from _client("sq6-demo", "sq6", connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture(scope="session")
def gob2_client() -> Iterator[McpClient]:
    """Gobliins 2 interactive demo (Gob engine, hotspots with hover names).

    Nothing like Gobliiins on the same engine: gob2 has per-object hotspots
    that name themselves in the status bar, so it goes down the same path
    Woodruff does. No save support; one ordered sequence."""
    yield from _client("gob2-demo", "gob2")


@pytest.fixture(scope="session")
def gob3_client() -> Iterator[McpClient]:
    """Goblins Quest 3 interactive demo (Gob engine, hotspots)."""
    yield from _client("gob3-demo", "gob3")


@pytest.fixture(scope="session")
def ween_client() -> Iterator[McpClient]:
    """Ween: The Prophecy demo (Gob engine, hotspots).

    Opens with minutes of video, so its tests skip their way in and give the
    opening a generous number of goes."""
    yield from _client("ween-demo", "ween")


@pytest.fixture(scope="session")
def cstime_client() -> Iterator[McpClient]:
    """Where in Time is Carmen Sandiego? (Mohawk engine, CSTime).

    A pointer game with no verbs at all: a scene is a picture with regions
    marked on it, and clicking one is the whole vocabulary. No save support."""
    yield from _client("cstime-demo", "cstime", connect_timeout=HUGE_TABLE_BOOT_SECS)


@pytest.fixture(scope="session")
def zak_repixeled_client() -> Iterator[McpClient]:
    """Zak McKracken - repixeled (AGS engine, verbs on a bar).

    A SCUMM-style fan game: its verbs are GUI buttons rather than cursor
    modes, which is what select_verb exists for. No save support."""
    yield from _client("zak-repixeled", "zak_repixeled", connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture(scope="session")
def zak_seamonster_client() -> Iterator[McpClient]:
    """Zak McKracken and the Lonely Sea Monster (AGS engine, cursor verbs).

    The other AGS shape: the verb is the cursor mode, so act() sets it
    itself and there is no bar. No save support."""
    yield from _client("zak-seamonster", "zak_seamonster", connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture(scope="session")
def gob1_client() -> Iterator[McpClient]:
    """Gobliiins interactive demo (Gob engine, three-character team).

    No save support: one ordered sequence on a single instance, started fresh
    and skipped past the intro (like woodruff and the atlantis/ft demos)."""
    yield from _client("gob1-demo", "gob1")


@pytest.fixture(scope="session")
def sword2_client() -> Iterator[McpClient]:
    """Broken Sword 2 demo (sword2 engine).

    No save to load: the demo starts on its own opening and hands over on the
    docks, so the whole run is one ordered sequence on a single instance."""
    yield from _client("sword2-demo", "sword2")


@pytest.fixture(scope="session")
def dw1_client() -> Iterator[McpClient]:
    """Discworld CD demo (Tinsel engine, V1).

    No save slot to load: the demo starts on its intro sequence, so the whole
    run is one ordered sequence on a single instance, skipped past the intro
    into Rincewind's bedroom (like woodruff and the atlantis/ft demos)."""
    yield from _client("dw1-demo", "dw1")


@pytest.fixture(scope="session")
def dw2_client() -> Iterator[McpClient]:
    """Discworld II demo (Tinsel engine, V2).

    Same shape as dw1_client: started fresh and skipped past the intro. Note
    that only the Windows demo runs under ScummVM — the DOS one is flagged
    unsupported by the engine, so pointing dw2-demo at it will not start."""
    yield from _client("dw2-demo", "dw2")


@pytest.fixture(scope="session")
def toon_client() -> Iterator[McpClient]:
    """Toonstruck demo (Toon engine).

    No save slot to load: the demo opens on a logo movie and then plays, so the
    whole run is one ordered sequence on a single instance, skipped past the
    opening the way the other fresh-start demos are."""
    yield from _client("toon-demo", "toon")


@pytest.fixture(scope="session")
def woodruff_client() -> Iterator[McpClient]:
    """The Bizarre Adventures of Woodruff and the Schnibble (Gob engine).

    No save support: the whole run is one ordered sequence on a single instance,
    started fresh and skipped past the intro (like the atlantis/ft demos)."""
    yield from _client("woodruff", "woodruff")

# ---------------------------------------------------------------------------
# The full games.
#
# Each starts from its own slot 1, captured just past the opening so the tests
# begin where a player would rather than in the middle of a film.
#
# Where one could be captured, that is: a good many of these games refuse to
# save anywhere in their opening and say so in those words - Loom, both
# Discworlds, Gobliins 2 and 3 and Ween were each asked for seventeen minutes
# and never relented. Those start from scratch and skip their own way in, and
# `has_captured_save` is what tells the two cases apart, so a game that learns
# to save tomorrow needs no edit here.
# ---------------------------------------------------------------------------


@pytest.fixture
def ft_full_client() -> Iterator[McpClient]:
    """Full Throttle (past the opening, slot 1)."""
    yield from _client("ft-full", "ft_full", checkpoint=has_captured_save("ft-full"))


@pytest.fixture
def loom_full_client() -> Iterator[McpClient]:
    """Loom (CD) (past the opening, slot 1)."""
    yield from _client("loom-full", "loom_full", checkpoint=has_captured_save("loom-full"))


@pytest.fixture
def indy3_full_client() -> Iterator[McpClient]:
    """Indiana Jones and the Last Crusade (past the opening, slot 1)."""
    yield from _client("indy3-full", "indy3_full", checkpoint=has_captured_save("indy3-full"))


@pytest.fixture
def atlantis_full_client() -> Iterator[McpClient]:
    """Indiana Jones and the Fate of Atlantis (past the opening, slot 1)."""
    yield from _client("atlantis-full", "atlantis_full", checkpoint=has_captured_save("atlantis-full"))


@pytest.fixture
def samnmax_full_client() -> Iterator[McpClient]:
    """Sam & Max Hit the Road (past the opening, slot 1)."""
    yield from _client("samnmax-full", "samnmax_full", checkpoint=has_captured_save("samnmax-full"))


@pytest.fixture
def monkey_full_client() -> Iterator[McpClient]:
    """The Secret of Monkey Island (Amiga) (past the opening, slot 1)."""
    yield from _client("monkey-full", "monkey_full", checkpoint=has_captured_save("monkey-full"))


@pytest.fixture
def dw_full_client() -> Iterator[McpClient]:
    """Discworld (past the opening, slot 1)."""
    yield from _client("dw-full", "dw_full", checkpoint=has_captured_save("dw-full"))


@pytest.fixture
def dw2_full_client() -> Iterator[McpClient]:
    """Discworld II (past the opening, slot 1)."""
    yield from _client("dw2-full", "dw2_full", checkpoint=has_captured_save("dw2-full"))


@pytest.fixture
def sword1_full_client() -> Iterator[McpClient]:
    """Broken Sword: The Shadow of the Templars (past the opening, slot 1)."""
    yield from _client("sword1-full", "sword1_full", checkpoint=has_captured_save("sword1-full"))


@pytest.fixture
def gob2_full_client() -> Iterator[McpClient]:
    """Gobliins 2 (past the opening, slot 1)."""
    yield from _client("gob2-full", "gob2_full", checkpoint=has_captured_save("gob2-full"))


@pytest.fixture
def gob3_full_client() -> Iterator[McpClient]:
    """Goblins Quest 3 (past the opening, slot 1)."""
    yield from _client("gob3-full", "gob3_full", checkpoint=has_captured_save("gob3-full"))


@pytest.fixture
def ween_full_client() -> Iterator[McpClient]:
    """Ween: The Prophecy (Amiga) (past the opening, slot 1)."""
    yield from _client("ween-full", "ween_full", checkpoint=has_captured_save("ween-full"))


@pytest.fixture
def gk1_full_client() -> Iterator[McpClient]:
    """Gabriel Knight: Sins of the Fathers (past the opening, slot 1)."""
    yield from _client("gk1-full", "gk1_full", checkpoint=has_captured_save("gk1-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def sq6_full_client() -> Iterator[McpClient]:
    """Space Quest 6 (past the opening, slot 1)."""
    yield from _client("sq6-full", "sq6_full", checkpoint=has_captured_save("sq6-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kq5_full_client() -> Iterator[McpClient]:
    """King's Quest V (past the opening, slot 1)."""
    yield from _client("kq5-full", "kq5_full", checkpoint=has_captured_save("kq5-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kq6_full_client() -> Iterator[McpClient]:
    """King's Quest VI (past the opening, slot 1)."""
    yield from _client("kq6-full", "kq6_full", checkpoint=has_captured_save("kq6-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kq7_full_client() -> Iterator[McpClient]:
    """King's Quest VII (past the opening, slot 1)."""
    yield from _client("kq7-full", "kq7_full", checkpoint=has_captured_save("kq7-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def sq4_full_client() -> Iterator[McpClient]:
    """Space Quest IV (past the opening, slot 1)."""
    yield from _client("sq4-full", "sq4_full", checkpoint=has_captured_save("sq4-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def sq5_full_client() -> Iterator[McpClient]:
    """Space Quest V (past the opening, slot 1)."""
    yield from _client("sq5-full", "sq5_full", checkpoint=has_captured_save("sq5-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def pq3_full_client() -> Iterator[McpClient]:
    """Police Quest III (past the opening, slot 1)."""
    yield from _client("pq3-full", "pq3_full", checkpoint=has_captured_save("pq3-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kq1sci_full_client() -> Iterator[McpClient]:
    """King's Quest I (SCI remake) (past the opening, slot 1)."""
    yield from _client("kq1sci-full", "kq1sci_full", checkpoint=has_captured_save("kq1sci-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kq4sci_full_client() -> Iterator[McpClient]:
    """King's Quest IV (past the opening, slot 1)."""
    yield from _client("kq4sci-full", "kq4sci_full", checkpoint=has_captured_save("kq4sci-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def sq1sci_full_client() -> Iterator[McpClient]:
    """Space Quest I (SCI remake) (past the opening, slot 1)."""
    yield from _client("sq1sci-full", "sq1sci_full", checkpoint=has_captured_save("sq1sci-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def qfg1_full_client() -> Iterator[McpClient]:
    """Hero's Quest (past the opening, slot 1)."""
    yield from _client("qfg1-full", "qfg1_full", checkpoint=has_captured_save("qfg1-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def qfg2_full_client() -> Iterator[McpClient]:
    """Quest for Glory II (past the opening, slot 1)."""
    yield from _client("qfg2-full", "qfg2_full", checkpoint=has_captured_save("qfg2-full"), connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def sq2vga_client() -> Iterator[McpClient]:
    """Space Quest II VGA remake (past the opening, slot 1)."""
    yield from _client("sq2vga", "sq2vga", checkpoint=has_captured_save("sq2vga"))


@pytest.fixture
def pq2_full_client() -> Iterator[McpClient]:
    """Police Quest II, Amiga (past the opening, slot 1)."""
    yield from _client("pq2-full", "pq2_full", checkpoint=has_captured_save("pq2-full"),
                       connect_timeout=SLOW_BOOT_SECS)

# ---------------------------------------------------------------------------
# The games on the engines taught MCP for them.
#
# Each starts from its own slot 1, captured past the opening. A game whose
# slot has not been captured on this machine skips rather than fails.
# ---------------------------------------------------------------------------


@pytest.fixture
def kq2_client() -> Iterator[McpClient]:
    """King's Quest II (past the opening, slot 1)."""
    yield from _client("kq2", "kq2", checkpoint=has_captured_save("kq2"),
                       connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kq3_client() -> Iterator[McpClient]:
    """King's Quest III (Amiga) (past the opening, slot 1)."""
    yield from _client("kq3", "kq3", checkpoint=has_captured_save("kq3"),
                       connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def pq1_client() -> Iterator[McpClient]:
    """Police Quest (past the opening, slot 1)."""
    yield from _client("pq1", "pq1", checkpoint=has_captured_save("pq1"),
                       connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kyra1_client() -> Iterator[McpClient]:
    """The Legend of Kyrandia (past the opening, slot 1)."""
    yield from _client("kyra1", "kyra1", checkpoint=has_captured_save("kyra1"),
                       connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kyra2_client() -> Iterator[McpClient]:
    """Kyrandia: The Hand of Fate (past the opening, slot 1)."""
    yield from _client("kyra2", "kyra2", checkpoint=has_captured_save("kyra2"),
                       connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def kyra3_client() -> Iterator[McpClient]:
    """Kyrandia: Malcolm's Revenge (past the opening, slot 1)."""
    yield from _client("kyra3", "kyra3", checkpoint=has_captured_save("kyra3"),
                       connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def simon1_client() -> Iterator[McpClient]:
    """Simon the Sorcerer (Amiga) (past the opening, slot 1)."""
    yield from _client("simon1", "simon1", checkpoint=has_captured_save("simon1"),
                       connect_timeout=SLOW_BOOT_SECS)


@pytest.fixture
def sanitarium_client() -> Iterator[McpClient]:
    """Sanitarium (past the opening, slot 1)."""
    yield from _client("sanitarium", "sanitarium", checkpoint=has_captured_save("sanitarium"),
                       connect_timeout=SLOW_BOOT_SECS)

