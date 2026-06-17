#!/usr/bin/env python3
"""Launch Sam & Max demo on a save slot and keep it alive for manual MCP driving."""
import os
import sys

from utils import GAME_PATHS, launch_scummvm

port = int(sys.argv[1]) if len(sys.argv) > 1 else 23499
slot = int(sys.argv[2]) if len(sys.argv) > 2 else 2

binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
proc = launch_scummvm("samnmax", GAME_PATHS["samnmax"], port=port, save_slot=slot,
                      scummvm_binary=binary)
print(f"[manual] scummvm pid={proc.pid} port={port} slot={slot}", flush=True)
try:
    proc.wait()
finally:
    print("[manual] scummvm exited", flush=True)
