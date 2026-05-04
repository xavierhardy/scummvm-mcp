"""Manual probe of Dig save scenario via MCP."""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))

from utils import GAME_PATHS, MCP_HOST, launch_scummvm, require_game_path, wait_for_mcp

PORT = 23560
require_game_path("dig-demo")
scummvm = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")


def dump(label, obj):
    s = json.dumps(obj, indent=2, default=str) if obj is not None else "None"
    print(f"\n=== {label} ===\n{s[:2000]}")


def act(client, label, **kwargs):
    notes, msgs, r = client.call_capturing("act", kwargs)
    dump(f"ACT {label}", {"args": kwargs, "result": r, "msgs": msgs})


def answer(client, label, choice):
    notes, msgs, r = client.call_capturing("answer", {"id": choice})
    dump(f"ANSWER {label} ({choice})", {"result": r, "msgs": msgs})


def show_state(client, label):
    s = client.state()
    dump(label, {
        "room": s.get("room"),
        "position": s.get("position"),
        "objects": [(o["name"], o.get("x"), o.get("y")) for o in s.get("objects", [])],
        "inventory": s.get("inventory"),
        "question": s.get("question"),
    })


proc = launch_scummvm("dig-demo", GAME_PATHS["dig-demo"], port=PORT, scummvm_binary=scummvm)
try:
    client = wait_for_mcp(MCP_HOST, PORT, timeout=15.0)
    time.sleep(2.0)
    show_state(client, "INITIAL")

    # Step 1: click on Brink → "Brink..." then dialog
    act(client, "interact brink", verb="interact", target1="brink")
    for i in range(8):
        time.sleep(1.0)
        s = client.state()
        print(f"  +{i+1}s question={bool(s.get('question'))} verbs={s.get('verbs')}")
        if s.get("question"):
            print("    choices:", s["question"]["choices"])
            break
    show_state(client, "AFTER BRINK")

    # Step 2: choice 1 (How are you doing?)
    answer(client, "how are you", 1)
    show_state(client, "AFTER ANSWER 1")

    # Step 3: choice 2 (eerie place)
    answer(client, "eerie", 2)
    show_state(client, "AFTER ANSWER 2")

    # Step 4: choice 3 (Stop)
    answer(client, "stop", 3)
    show_state(client, "AFTER ANSWER 3")

    # Step 5: click on platform
    act(client, "interact platform", verb="interact", target1="platform")
    show_state(client, "AFTER PLATFORM")

    # Step 6: use trowel on Maggie
    act(client, "use trowel/maggie", verb="use item", target1="trowel", target2="maggie")
    show_state(client, "AFTER USE TROWEL MAGGIE")

    # Step 7: walk to clearing (id 53 — the lower-right one)
    act(client, "interact clearing(53)", verb="interact", target1=53)
    show_state(client, "AFTER CLEARING")
finally:
    proc.kill()
    proc.wait(timeout=5)
