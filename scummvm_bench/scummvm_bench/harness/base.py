"""Harness runner protocol, the shared run context, the attach (`none`) mode,
and the shared agent-subprocess launcher used by the coding harnesses."""

import subprocess
import threading
import time
from dataclasses import dataclass
from typing import IO, Protocol

from ..models import RunSpec
from ..proxy import BenchProxy

# How long the attach mode waits for a stopping goal / limit before giving up.
DEFAULT_ATTACH_WAIT_S = 600.0
# Extra wall-clock granted on top of the time limit before force-killing an agent.
KILL_GRACE_S = 30.0
# Fallback deadline used when the spec sets no explicit time limit.
DEFAULT_DEADLINE_S = 1800.0


@dataclass
class RunContext:
    """Everything a harness needs to drive one run."""

    spec: RunSpec
    bench_port: int
    agent_dir: str
    stop_event: threading.Event
    proxy: BenchProxy
    # Backend-side facts the coding harnesses need to build the sandbox jail.
    # ``backend_dir`` holds the rendered ini/logs (and so leaks ``scummvm_port``)
    # and is denied to the agent; ``game_path``/``save_folder`` are the data dirs
    # to hide. Per-harness config dirs isolate the agent's config from the user's.
    backend_dir: str = ""
    scummvm_port: int = 0
    game_path: str | None = None
    save_folder: str | None = None
    pi_config_dir: str | None = None
    claude_config_dir: str | None = None


class HarnessRunner(Protocol):
    """Drives the agent against the bench proxy and blocks until it is done."""

    def run(self, ctx: RunContext) -> str | None: ...


class NoneHarness:
    """Attach mode: bring the proxy up and let an external/human agent drive.

    Blocks until the recorder signals a stop (stopping goal or limit) or a
    deadline elapses. Used when the coding harness cannot be launched
    programmatically.
    """

    def __init__(self, max_wait_s: float = DEFAULT_ATTACH_WAIT_S) -> None:
        self.max_wait_s = max_wait_s

    def run(self, ctx: RunContext) -> str | None:
        timeout = ctx.spec.time_limit_s or self.max_wait_s
        ctx.stop_event.wait(timeout)
        return None


def _pump_stream(stream: IO[str] | None, transcript: IO[str]) -> None:
    """Copy the agent's output stream into the transcript file, line by line."""
    if stream is None:
        return
    for line in stream:
        transcript.write(line)
        transcript.flush()


def launch_agent(
    ctx: RunContext,
    args: list[str],
    jsonl_path: str,
    env: dict[str, str],
    *,
    label: str,
) -> str | None:
    """Launch a coding-agent subprocess and block until it is done.

    Streams the process's combined output to ``jsonl_path`` and stops on the
    first of: the process exiting, ``ctx.stop_event`` firing (a stopping goal or
    limit), or the deadline (the spec's time limit plus a kill grace). Returns
    ``None`` on a clean finish or a kill, or an error string when the process
    exited non-zero; ``label`` names the agent in that message (e.g. ``"pi"``).
    """
    deadline = time.monotonic() + (ctx.spec.time_limit_s or DEFAULT_DEADLINE_S)
    deadline += KILL_GRACE_S
    with open(jsonl_path, "w") as transcript:
        proc = subprocess.Popen(
            args,
            cwd=ctx.agent_dir,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        pump = threading.Thread(
            target=_pump_stream, args=(proc.stdout, transcript), daemon=True
        )
        pump.start()

        killed = False
        while proc.poll() is None:
            if ctx.stop_event.wait(0.2) or time.monotonic() > deadline:
                proc.kill()
                killed = True
                break
        try:
            proc.wait(timeout=5)
        except Exception:  # noqa: BLE001
            pass
        pump.join(timeout=2)

    if killed:
        return None
    if proc.returncode not in (0, None):
        return f"{label} exited with code {proc.returncode}"
    return None
