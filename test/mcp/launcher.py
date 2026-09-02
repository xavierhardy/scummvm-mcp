#!/usr/bin/env python3
"""Headless ScummVM launcher + game-data/save-slot resolution for the tests.

Renders a per-game ini, isolates saves, and spawns ScummVM with MCP enabled, plus
the ``GAME_PATHS`` map and the ``require_*`` skip guards. Imported via ``utils``
for back-compat.

Game data lives outside the repository and sits somewhere different on every
machine, so no game-data path belongs in tracked code. ``GAME_PATHS`` is read
from the non-committed ``game_paths.local.toml`` at the repository root (see
``game_paths.local.toml.example``), optionally overridden per game by the
environment variables in ``_GAME_PATH_ENV``. A game configured by neither, or
pointed at a folder that does not exist, skips its tests.
"""

import os
import re
import shutil
import subprocess
import tempfile
import tomllib


def _logs_dir() -> str:
    """Return the (created) directory ScummVM's log files are written to."""
    logs_dir = os.path.join(os.path.dirname(__file__), "logs")
    os.makedirs(logs_dir, exist_ok=True)
    return logs_dir


def _log_tag(game_id: str, port: int) -> str:
    """The stem shared by one launch's log files.

    The port alone is not unique: it is allocated per (worker, fixture), so
    every function-scoped test using a fixture reuses it and each launch used
    to truncate the previous test's logs. By the time a failure was read, the
    log named in its output belonged to whichever test ran last — which is
    exactly the evidence a flaky failure needs. Appending the test name keeps
    one file per launch and makes it self-identifying.
    """
    # "test/mcp/test_tentacle.py::test_open_clock (setup)" -> "test_open_clock"
    current = os.environ.get("PYTEST_CURRENT_TEST", "")
    name = current.split("::")[-1].split(" ")[0] if current else ""
    safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in name)
    return f"scummvm_{game_id}_{port}" + (f"_{safe}" if safe else "")


def screenshot_dir(ini_path: str) -> str:
    """The folder the ``screenshot`` debug tool writes into for this instance.

    It sits next to the rendered ini, so it is private to one launch and needs
    no per-machine path (see ``_write_ini``).
    """
    return os.path.join(os.path.dirname(ini_path), "screenshots")


def _apply_ini_overrides(content: str, overrides: dict[str, str]) -> str:
    """Return *content* with each ``key=value`` replaced (or added).

    Used to launch a game with a different MCP configuration than its committed
    ini asks for — e.g. with the optional tools turned off, to check that the
    server then neither offers nor mentions them.
    """
    for key, value in overrides.items():
        line = f"{key}={value}"
        pattern = re.compile(rf"^{re.escape(key)}=.*$", re.MULTILINE)
        content, replaced = pattern.subn(line, content)
        if not replaced:
            content = content.rstrip("\n") + f"\n{line}\n"
    return content


def _write_ini(
    game_id: str,
    game_path: str,
    port: int,
    scummvm_log: str,
    ini_overrides: dict[str, str] | None = None,
) -> str:
    """Render the per-game ini template into a fresh temp dir, return its path."""
    ini_template = os.path.join(
        os.path.dirname(__file__), "ini_files", f"scummvm_{game_id}.ini"
    )
    tmpdir = tempfile.mkdtemp(prefix=f"scummvm_{game_id}_")
    ini_path = os.path.join(tmpdir, "scummvm.ini")
    shots = screenshot_dir(ini_path)
    os.makedirs(shots, exist_ok=True)
    with open(ini_template) as ini_file:
        content = ini_file.read() % {
            "game_path": game_path,
            "mcp_port": port,
            "logfile": scummvm_log,
            # Where the screenshot debug tool drops its PNGs: private to this
            # instance, so parallel runs never share a folder.
            "screenshot_path": shots,
            # The repo's engine-data dir, for engines that need a data file
            # from the ScummVM distribution (queen.tbl for FOTAQ).
            "extra_path": os.path.normpath(
                os.path.join(
                    os.path.dirname(__file__), "..", "..", "dists", "engine-data"
                )
            ),
        }
    # Before it creates the engine at all, ScummVM stops on a modal dialog for
    # a game its detection calls unsupported, and waits there to be told to go
    # on. Headless there is nobody to tell it, so the game never starts and the
    # port it would bind never opens. The harness always says go on.
    if "[scummvm]" in content:
        content = content.replace(
            "[scummvm]\n",
            "[scummvm]\nenable_unsupported_game_warning=false\n",
            1,
        )
    else:
        content = "[scummvm]\nenable_unsupported_game_warning=false\n\n" + content
    if ini_overrides:
        content = _apply_ini_overrides(content, ini_overrides)
    with open(ini_path, "w") as f:
        f.write(content)
    return ini_path


def _resolve_save_path(game_id: str, ini_path: str, isolate_saves: bool) -> str:
    """Return the ``--savepath`` for this instance.

    When ``isolate_saves`` is true, copy the repo's ``save_slots/<game_id>`` into
    a private folder (next to the ini) so concurrent same-game instances never
    share (and clobber) save files; otherwise use the repo directory directly.
    """
    repo_save_path = os.path.join(os.path.dirname(__file__), f"save_slots/{game_id}")
    if not isolate_saves:
        return repo_save_path
    save_path = os.path.join(os.path.dirname(ini_path), "saves")
    if os.path.isdir(repo_save_path):
        shutil.copytree(repo_save_path, save_path)
    else:
        os.makedirs(save_path, exist_ok=True)
    return save_path


def has_captured_save(game_id: str) -> bool:
    """Whether a save slot for *game_id* has been captured into the repository.

    A game with none has to start from scratch: passing --save-slot for a save
    that is not there leaves the engine sitting on an error, which headless is
    a hang.
    """
    folder = os.path.join(os.path.dirname(__file__), "save_slots", game_id)
    if not os.path.isdir(folder):
        return False
    return any(
        not name.startswith(".") and name != "timestamps" for name in os.listdir(folder)
    )


def _launch_args(
    game_id: str,
    scummvm_binary: str,
    ini_path: str,
    save_slot: int,
    save_path: str,
) -> list[str]:
    """Build the ScummVM command line for ``game_id``."""
    if not has_captured_save(game_id):
        # No save to start from, so start from scratch and let the game handle
        # its own opening.
        #
        # This used to be a list of the games that could not save, which had to
        # be added to every time another one turned up — and a great many of
        # them do. Several of the full games refuse to save anywhere in their
        # opening (Loom, both Discworlds, Gobliins 2 and 3 were all asked for
        # seventeen minutes and answered "cannot be saved in the current state"
        # the whole way), and every demo here has always refused. Whether a
        # save was captured is the same question and answers itself.
        #
        # The isolated (empty) save folder still goes on the line: without it
        # the game writes into — and reads — whatever save folder this machine
        # has, and an engine that offers to restore whenever a save exists
        # (Broken Sword 2, which writes its own autosave) then stops on that
        # dialog before its first game cycle and blocks a headless run forever.
        return [scummvm_binary, "-c", ini_path, f"--savepath={save_path}", game_id]
    return [
        scummvm_binary,
        "-c",
        ini_path,
        f"--save-slot={save_slot}",
        f"--savepath={save_path}",
        "--talkspeed=255",
        game_id,
    ]


def _open_log_handles(game_id: str, port: int, args: list[str], ini_path: str):
    """Write a header to the stdout log and return ``(stdout_fh, stderr_fh,
    log_file, stderr_file)`` append handles for the launched process."""
    logs_dir = _logs_dir()
    tag = _log_tag(game_id, port)
    log_file = os.path.join(logs_dir, f"{tag}.log")
    stderr_file = os.path.join(logs_dir, f"{tag}.stderr")
    with open(log_file, "w") as logf:
        logf.write(f"Command: {' '.join(args)}\n")
        logf.write("Environment: SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy\n")
        logf.write(f"Config: {ini_path}\n")
        logf.write("=" * 80 + "\n\n")
    return open(log_file, "a"), open(stderr_file, "a"), log_file, stderr_file


def launch_scummvm(
    game_id: str,
    game_path: str,
    port: int = 23456,
    scummvm_binary: str = "./scummvm",
    save_slot: int = 1,
    isolate_saves: bool = True,
    ini_overrides: dict[str, str] | None = None,
) -> subprocess.Popen:
    """Launch ScummVM headlessly with MCP enabled for the given game.

    When ``isolate_saves`` is true (the default, used by the test fixtures), the
    game's ``save_slots/<game_id>`` directory is copied into a private temporary
    folder used as the ``--savepath``. This keeps parallel instances of the same
    game from racing on a shared save directory and stops autosaves from
    polluting the committed save files. Pass ``isolate_saves=False`` (e.g. from
    ``launch_manual.py``) when you want ``save_state`` to write back into the
    repository's ``save_slots/<game_id>`` directory."""
    scummvm_log = os.path.join(_logs_dir(), f"{_log_tag(game_id, port)}.scummvm.log")
    ini_path = _write_ini(game_id, game_path, port, scummvm_log, ini_overrides)
    save_path = _resolve_save_path(game_id, ini_path, isolate_saves)
    args = _launch_args(game_id, scummvm_binary, ini_path, save_slot, save_path)

    # Launch with no video/audio. Both drivers matter once instances run in
    # parallel: a real audio device is exclusive, so every instance after the
    # first blocks and never binds its MCP port, and a real window leaves the
    # compositor pacing the engine — four windows measured 1 game frame/s
    # instead of 15. Every streaming budget in the bridge is counted in engine
    # frames, so at 1 fps an action that normally settles in a second outlives
    # the client's HTTP timeout and the test fails as a spurious hang.
    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = "dummy"
    env["SDL_VIDEODRIVER"] = "dummy"

    stdout_file, stderr_fh, log_file, stderr_file = _open_log_handles(
        game_id, port, args, ini_path
    )
    # Run from test/mcp regardless of the caller's CWD: ScummVM resolves some
    # runtime state relative to the working directory, and the suite historically
    # ran from here. (Tests may now be driven from the consolidated project in
    # ../scummvm_bench, so don't inherit that CWD.)
    proc = subprocess.Popen(
        args,
        env=env,
        stdout=stdout_file,
        stderr=stderr_fh,
        cwd=os.path.dirname(__file__),
    )

    # Keep the log file handles alive for the process lifetime so they are not
    # garbage-collected; the fixture teardown closes them (see conftest.py).
    # setattr (not attribute assignment) because Popen has no typed slot for it.
    setattr(proc, "_log_handles", (stdout_file, stderr_fh))  # noqa: B010
    # Where this instance's screenshot tool writes, for tests that check it.
    setattr(proc, "screenshot_path", screenshot_dir(ini_path))  # noqa: B010

    print(f"[MCP] {game_id} stdout: {log_file}", flush=True)
    print(f"[MCP] {game_id} stderr: {stderr_file}", flush=True)

    return proc


# game id -> the environment variable that can point at its data folder. The
# folders themselves are per-machine and are deliberately NOT in this file: they
# live in the non-committed game_paths.local.toml (see the .example file), or
# come from the env var. A game with neither configured skips its tests.
_GAME_PATH_ENV = {
    "monkey-ega-demo": "MONKEY_DEMO_PATH",
    "monkey-ega-demo-de": "MONKEY_DEMO_DE_PATH",
    "maniac-c64": "MANIAC_C64_PATH",
    "maniac": "MANIAC_PATH",
    "zak": "ZAK_PATH",
    "pass": "PASS_DEMO_PATH",
    "monkey2": "MONKEY2_PATH",
    "atlantis": "ATLANTIS_DEMO_PATH",
    "samnmax": "SAMNMAX_DEMO_PATH",
    "tentacle": "TENTACLE_PATH",
    "ft-demo": "FT_DEMO_PATH",
    "dig-demo": "DIG_DEMO_PATH",
    "comi-demo": "COMI_DEMO_PATH",
    "sword1-demo": "SWORD1_DEMO_PATH",
    "sky": "SKY_PATH",
    "queen": "QUEEN_PATH",
    "woodruff": "WOODRUFF_PATH",
    "gob1-demo": "GOB1_DEMO_PATH",
    "dw1-demo": "DW1_DEMO_PATH",
    "dw2-demo": "DW2_DEMO_PATH",
    "sword2-demo": "SWORD2_DEMO_PATH",
    "toon-demo": "TOON_DEMO_PATH",
    "gk1-demo": "GK1_DEMO_PATH",
    "sq6-demo": "SQ6_DEMO_PATH",
    "gob2-demo": "GOB2_DEMO_PATH",
    "gob3-demo": "GOB3_DEMO_PATH",
    "ween-demo": "WEEN_DEMO_PATH",
    "zak-repixeled": "ZAK_REPIXELED_PATH",
    "zak-seamonster": "ZAK_SEAMONSTER_PATH",
    "cstime-demo": "CSTIME_DEMO_PATH",
}

LOCAL_PATHS_FILE = os.path.normpath(
    os.path.join(os.path.dirname(__file__), "..", "..", "game_paths.local.toml")
)


def _local_game_paths() -> dict[str, str]:
    """Return the per-machine game folders from the non-committed local config.

    The repository root's ``game_paths.local.toml`` (or whatever
    ``MCP_GAME_PATHS_FILE`` points at) is a ``[games]`` table of game id ->
    folder::

        [games]
        sky = "~/games/sky/Contents/Resources/game/game"

    It is gitignored, which is the whole point: game data is not in the tree and
    where it sits differs per machine, so no path belongs in tracked code. An
    absent file — the normal case on a machine with no game data — just means no
    game is configured and every game test skips.
    """
    path = os.environ.get("MCP_GAME_PATHS_FILE") or LOCAL_PATHS_FILE
    if not os.path.isfile(path):
        return {}
    with open(path, "rb") as config_file:
        raw = tomllib.load(config_file)
    games = raw.get("games", {})
    if not isinstance(games, dict):
        raise ValueError(f"{path}: [games] must be a table of game id -> folder")
    return {game_id: os.path.expanduser(str(f)) for game_id, f in games.items()}


def _resolve_game_paths() -> dict[str, str]:
    """Folders from the local config, overridden by the per-game env vars.

    Games configured by neither are left out of the map entirely, so
    :func:`require_game_path` skips them with a pointer to the local config.
    """
    local = _local_game_paths()
    resolved = dict(local)
    for game_id, env_var in _GAME_PATH_ENV.items():
        folder = os.environ.get(env_var)
        if folder:
            resolved[game_id] = os.path.expanduser(folder)
    return resolved


GAME_PATHS = _resolve_game_paths()

# Save-file naming is per engine: SCUMM writes "<target>.sNN", while sword1
# writes "sword1.NNN" (SwordEngine::getSaveStateName) and sky writes
# "SKY-VM.NNN" (SkyEngine::getSaveStateName). Games not listed here use the
# SCUMM form ("queen.sNN" matches it already).
_SAVE_NAME_FMT = {
    "sword1-demo": "sword1.{slot:03d}",
    "sky": "SKY-VM.{slot:03d}",
}


def require_game_path(game_id: str) -> None:
    """Skip the test unless *game_id*'s data folder is configured and present."""
    import pytest

    path = GAME_PATHS.get(game_id)
    if not path:
        env_var = _GAME_PATH_ENV.get(game_id, f"<{game_id} path env var>")
        pytest.skip(
            f"{game_id} has no data folder configured — add it under [games] in "
            f"{LOCAL_PATHS_FILE} (see game_paths.local.toml.example) or set {env_var}"
        )
    if not os.path.isdir(path):
        pytest.skip(f"{game_id} game data not found at {path}")


def save_slot_path(game_id: str, slot: int) -> str:
    """Path to the committed save file for *game_id*'s *slot*.

    Every engine names its saves differently — ``monkey.s01``, ``sword1.001``,
    ``SKY-VM.001``, ``kyra1.000`` — and the fifteen engines here between them
    use most of the spellings there are. Rather than keep a table of them, the
    SCUMM form is tried first (it is much the commonest) and then the folder is
    asked: a save-slot folder holds this game's save and nothing else, so a
    file sitting in it *is* the save whatever the engine chose to call it.
    """
    folder = os.path.join(os.path.dirname(__file__), "save_slots", game_id)
    fmt = _SAVE_NAME_FMT.get(game_id, "{game_id}.s{slot:02d}")
    named = os.path.join(folder, fmt.format(game_id=game_id, slot=slot))
    if os.path.isfile(named) or not os.path.isdir(folder):
        return named
    saves = sorted(
        name
        for name in os.listdir(folder)
        # `timestamps` is ScummVM's own bookkeeping, not a save.
        if not name.startswith(".") and name != "timestamps"
    )
    return os.path.join(folder, saves[0]) if saves else named


def require_save_slot(game_id: str, slot: int) -> None:
    """Skip test if the checkpoint save file for this slot hasn't been captured.

    Checkpoint slots are generated by ``make_save_states.py`` on a machine that
    has the game data; until that capture is run, the slot file is absent and
    the dependent tests skip rather than fail.
    """
    import pytest

    path = save_slot_path(game_id, slot)
    if not os.path.isfile(path):
        pytest.skip(
            f"checkpoint save {os.path.basename(path)} not found — run "
            f"make_save_states.py to capture it"
        )
