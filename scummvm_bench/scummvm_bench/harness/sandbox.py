"""Process jail + environment scrub shared by the coding-agent harnesses.

A benchmarked agent is supposed to play the game *only* through the MCP proxy.
Restricting its tool list (``pi --no-builtin-tools`` / ``claude --tools``) keeps
it on the proxy, but a determined agent could still shell out to read the answer
key (``goals/*.py``, ``tests/walkthroughs/*.py``), the game data/saves, or talk
to the raw ScummVM MCP server directly. On macOS we wrap the agent process in
``sandbox-exec`` so those paths and that port are unreachable no matter what
tools survive; everywhere else the wrapper is a no-op (with a warning) and we
fall back to the tool-restriction layer.

We also build a minimal environment for the agent so it cannot read the bench's
game-path variables (which point straight at the data dirs) or exfiltrate
unrelated secrets.
"""

import logging
import os
import shlex
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path

logger = logging.getLogger(__name__)

# The bench package root and the repo paths that hold the "answer key": the goal
# predicates and the captured walkthroughs. Denied to the agent on macOS.
_PACKAGE_ROOT = Path(__file__).resolve().parents[1]
_ANSWER_KEY_PATHS = (
    _PACKAGE_ROOT / "goals",
    _PACKAGE_ROOT.parent / "tests",
)

# Environment variables the agent process is allowed to inherit. Anything else
# (notably the bench's ``*_PATH`` game-data variables) is dropped. Provider API
# keys are matched by suffix below so every provider keeps working.
_ENV_KEEP_EXACT = frozenset(
    {
        "PATH",
        "HOME",
        "USER",
        "LOGNAME",
        "SHELL",
        "TERM",
        "COLORTERM",
        "TMPDIR",
        "TMP",
        "TEMP",
        "LANG",
        "SSL_CERT_FILE",
        "SSL_CERT_DIR",
        "NODE_EXTRA_CA_CERTS",
    }
)
_ENV_KEEP_PREFIXES = ("LC_",)
_ENV_KEEP_SUFFIXES = ("_API_KEY",)


def build_agent_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    """Return a scrubbed copy of the environment for an agent subprocess.

    Keeps a small allow-list plus any provider ``*_API_KEY`` so the model can
    authenticate, drops everything else, then applies ``extra`` (e.g. the
    per-run config-dir override) on top.
    """
    env: dict[str, str] = {}
    for key, value in os.environ.items():
        if (
            key in _ENV_KEEP_EXACT
            or key.startswith(_ENV_KEEP_PREFIXES)
            or key.endswith(_ENV_KEEP_SUFFIXES)
        ):
            env[key] = value
    if extra:
        env.update(extra)
    return env


def wrap_command(
    cmd: list[str],
    *,
    backend_dir: str,
    scummvm_port: int,
    deny_paths: Iterable[str | None] = (),
) -> list[str]:
    """Wrap ``cmd`` in ``sandbox-exec`` on macOS; return it unchanged elsewhere.

    The profile keeps the agent's default access (so the runtime, the LLM
    provider, and the bench proxy keep working) but denies reading the bench
    answer key, the game data/saves, and ``backend_dir`` (which holds the
    rendered ini leaking the ScummVM port), and denies any network connection to
    that port so the proxy cannot be bypassed.
    """
    if sys.platform != "darwin":
        logger.warning(
            "sandbox-exec is unavailable on %s; relying on tool restriction only",
            sys.platform,
        )
        return cmd

    profile = _build_profile(
        backend_dir=backend_dir, scummvm_port=scummvm_port, deny_paths=deny_paths
    )
    # The profile names the ScummVM port; write it into backend_dir, which the
    # profile itself denies the child from reading.
    handle = tempfile.NamedTemporaryFile(
        mode="w",
        prefix="sandbox_",
        suffix=".sb",
        dir=backend_dir,
        delete=False,
    )
    with handle:
        handle.write(profile)
    return ["sandbox-exec", "-f", handle.name, *cmd]


def _build_profile(
    *,
    backend_dir: str,
    scummvm_port: int,
    deny_paths: Iterable[str | None],
) -> str:
    """Render the SBPL profile. ``allow default`` then targeted denies; in SBPL
    the last matching rule wins, so the denies override the blanket allow."""
    subpaths: list[str] = []
    for path in (*_ANSWER_KEY_PATHS, backend_dir, *deny_paths):
        if not path:
            continue
        resolved = str(Path(str(path)).resolve())
        subpaths.append(f"  (subpath {_sbpl_str(resolved)})")
    deny_read = "\n".join(subpaths)
    # SBPL only accepts ``*`` or ``localhost`` as the host in a network address;
    # the IP literal is rejected. ``localhost`` matches loopback connections
    # (incl. ones dialled as 127.0.0.1), which is how the bench reaches ScummVM.
    return (
        "(version 1)\n"
        "(allow default)\n"
        ";; cannot reach the raw ScummVM MCP server -> no proxy bypass\n"
        f'(deny network* (remote tcp "localhost:{scummvm_port}"))\n'
        ";; cannot read the answer key, the game data/saves, or the ini/logs\n"
        f"(deny file-read*\n{deny_read})\n"
    )


def _sbpl_str(value: str) -> str:
    """Quote a string for an SBPL literal."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def shell_preview(cmd: list[str]) -> str:
    """Best-effort shell rendering of a command, for logs/debugging."""
    return " ".join(shlex.quote(part) for part in cmd)
