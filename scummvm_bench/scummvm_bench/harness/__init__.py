"""Harness registry.

Add a harness by implementing :class:`HarnessRunner` and registering a factory
here. ``none`` is the attach-after-the-fact mode.
"""

from collections.abc import Callable

from .base import HarnessRunner, NoneHarness, RunContext
from .pi import PiHarness

HARNESSES: dict[str, Callable[[], HarnessRunner]] = {
    "pi": PiHarness,
    "none": NoneHarness,
}


def make_harness(name: str) -> HarnessRunner:
    """Instantiate the harness runner registered under ``name``."""
    try:
        factory = HARNESSES[name]
    except KeyError:
        raise KeyError(
            f"unknown harness {name!r}; known: {sorted(HARNESSES)}"
        ) from None
    return factory()


__all__ = [
    "HARNESSES",
    "HarnessRunner",
    "NoneHarness",
    "PiHarness",
    "RunContext",
    "make_harness",
]
