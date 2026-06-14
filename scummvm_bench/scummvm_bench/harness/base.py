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
    session_dir: str
    stop_event: threading.Event
    proxy: BenchProxy


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
