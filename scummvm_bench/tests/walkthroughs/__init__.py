"""Per-game scripted walkthroughs for the mock and real full-run bench tests.

Each :class:`Walkthrough` carries the exact call sequence (replayed verbatim
against either a scripted ``MockBackend`` or a live ScummVM), the scripted
backend responses that mirror the real game, and where the game data lives. The
sequences were all reconciled against live captures of the demos.

Monkey Island has its own dedicated tests (``mock_harness.py`` +
``test_full_run_*_monkey.py``); this registry covers the additional games.

The walkthroughs are split one module per game; this package assembles them into
the :data:`WALKTHROUGHS` registry the bench tests import.
"""

from ._base import REPO, RealWalkthroughHarness, Walkthrough
from .comi import COMI
from .dig import DIG
from .indy import INDY3
from .loom import LOOM
from .maniac import MANIAC
from .samnmax import SAMNMAX

WALKTHROUGHS: dict[str, Walkthrough] = {
    "maniac-c64": MANIAC,
    "comi-demo": COMI,
    "samnmax": SAMNMAX,
    "dig-demo": DIG,
    "loom": LOOM,
    "indy3": INDY3,
}

__all__ = ["REPO", "WALKTHROUGHS", "RealWalkthroughHarness", "Walkthrough"]
