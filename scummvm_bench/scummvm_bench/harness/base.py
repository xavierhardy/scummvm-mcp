"""Harness runner protocol, the shared run context, and the attach (`none`) mode."""

import threading
from dataclasses import dataclass
from typing import Protocol

from ..models import RunSpec
from ..proxy import BenchProxy

# How long the attach mode waits for a stopping goal / limit before giving up.
DEFAULT_ATTACH_WAIT_S = 600.0


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
