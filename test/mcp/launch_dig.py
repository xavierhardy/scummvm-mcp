"""Launch The Dig demo and tail the log. Press Ctrl-C to quit."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from utils import GAME_PATHS, MCP_HOST, launch_scummvm, require_game_path, wait_for_mcp

PORT = 23560
require_game_path("dig-demo")
scummvm = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")

proc = launch_scummvm("dig-demo", GAME_PATHS["dig-demo"], port=PORT, scummvm_binary=scummvm)

log_path = os.path.join(os.path.dirname(__file__), "logs", f"scummvm_dig-demo_{PORT}.log")
print(f"[launch_dig] Game started (pid={proc.pid})")
print(f"[launch_dig] Tailing log: {log_path}")
print("[launch_dig] Press Ctrl-C to stop.\n")

# Wait for log file to appear
for _ in range(30):
    if os.path.exists(log_path):
        break
    time.sleep(0.2)

try:
    with open(log_path, "r") as f:
        f.seek(0, 2)  # start from end
        while proc.poll() is None:
            line = f.readline()
            if line:
                print(line, end="", flush=True)
            else:
                time.sleep(0.05)
except KeyboardInterrupt:
    pass
finally:
    print("\n[launch_dig] Stopping game.")
    proc.kill()
    proc.wait(timeout=5)
