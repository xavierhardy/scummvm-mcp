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
import shutil
import subprocess
import tempfile
import tomllib


def _logs_dir() -> str:
    """Return the (created) directory ScummVM's log files are written to."""
    logs_dir = os.path.join(os.path.dirname(__file__), "logs")
    os.makedirs(logs_dir, exist_ok=True)
    return logs_dir


def _write_ini(game_id: str, game_path: str, port: int, scummvm_log: str) -> str:
    """Render the per-game ini template into a fresh temp dir, return its path."""
    ini_template = os.path.join(
        os.path.dirname(__file__), "ini_files", f"scummvm_{game_id}.ini"
    )
    with open(ini_template) as ini_file:
        content = ini_file.read() % {
            "game_path": game_path,
            "mcp_port": port,
            "logfile": scummvm_log,
            # The repo's engine-data dir, for engines that need a data file
            # from the ScummVM distribution (queen.tbl for FOTAQ).
            "extra_path": os.path.normpath(
                os.path.join(
                    os.path.dirname(__file__), "..", "..", "dists", "engine-data"
                )
            ),
        }
    tmpdir = tempfile.mkdtemp(prefix=f"scummvm_{game_id}_")
    ini_path = os.path.join(tmpdir, "scummvm.ini")
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


def _launch_args(
    game_id: str,
    scummvm_binary: str,
    ini_path: str,
    save_slot: int,
    save_path: str,
) -> list[str]:
    """Build the ScummVM command line for ``game_id``."""
    if game_id in ("atlantis", "maniac", "woodruff"):
        # No save slot — these games start from scratch and handle their own intro.
        return [scummvm_binary, "-c", ini_path, game_id]
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
    log_file = os.path.join(logs_dir, f"scummvm_{game_id}_{port}.log")
    stderr_file = os.path.join(logs_dir, f"scummvm_{game_id}_{port}.stderr")
    with open(log_file, "w") as logf:
        logf.write(f"Command: {' '.join(args)}\n")
        logf.write("Environment: SDL_AUDIODRIVER=dummy\n")
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
) -> subprocess.Popen:
    """Launch ScummVM headlessly with MCP enabled for the given game.

    When ``isolate_saves`` is true (the default, used by the test fixtures), the
    game's ``save_slots/<game_id>`` directory is copied into a private temporary
    folder used as the ``--savepath``. This keeps parallel instances of the same
    game from racing on a shared save directory and stops autosaves from
    polluting the committed save files. Pass ``isolate_saves=False`` (e.g. from
    ``launch_manual.py``) when you want ``save_state`` to write back into the
    repository's ``save_slots/<game_id>`` directory."""
    scummvm_log = os.path.join(_logs_dir(), f"scummvm_{game_id}_{port}.scummvm.log")
    ini_path = _write_ini(game_id, game_path, port, scummvm_log)
    save_path = _resolve_save_path(game_id, ini_path, isolate_saves)
    args = _launch_args(game_id, scummvm_binary, ini_path, save_slot, save_path)

    # Launch with no video/audio
    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = "dummy"

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

    Defaults to the SCUMM ``<game>.sNN`` form; see ``_SAVE_NAME_FMT`` for the
    engines that name their saves differently.
    """
    fmt = _SAVE_NAME_FMT.get(game_id, "{game_id}.s{slot:02d}")
    return os.path.join(
        os.path.dirname(__file__),
        "save_slots",
        game_id,
        fmt.format(game_id=game_id, slot=slot),
    )


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
