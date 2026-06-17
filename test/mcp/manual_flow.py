#!/usr/bin/env python3
"""Manually drive Sam & Max: pick up Max, use him on the kitten. Logs VAR(177)."""
import json
import sys
import time

from utils import MCP_HOST, wait_for_mcp
from mcp_cli import _post_tool

port = int(sys.argv[1]) if len(sys.argv) > 1 else 23499
client = wait_for_mcp(MCP_HOST, port, timeout=10)


def var177():
    d = _post_tool(client, "debug", {"from": 173, "to": 177})
    vals = {v["i"]: v["v"] for v in d["vars"]}
    return vals[177], vals[173]


def actor_pos(actor_id):
    s = client.state()
    for o in s["objects"]:
        if o["id"] == actor_id:
            return o["x"], o["y"]
    return None


def hover(x, y):
    _post_tool(client, "mouse_move", {"x": x, "y": y})
    time.sleep(0.4)


def left():
    _post_tool(client, "mouse_click", {"x": -1, "y": -1} if False else {"x": cur[0], "y": cur[1]})


cur = (0, 0)

def click(x, y, button="left"):
    args = {"x": x, "y": y}
    if button != "left":
        args["button"] = button
    _post_tool(client, "mouse_click", args)
    time.sleep(0.4)


print("start var177/173:", var177())

# Phase 1: hover Max, cycle until pickup highlight (890) or use (878), then click
mx, my = actor_pos(3)
print("max at", mx, my)
hover(mx, my - 8)
print("over max:", var177())
for i in range(12):
    v, _ = var177()
    if v in (890, 878):
        break
    click(mx, my - 8, "right")
    mx, my = actor_pos(3)
    hover(mx, my - 8)
    print("cycle ->", var177())
v, _ = var177()
print("clicking max with cursor", v)
click(mx, my - 8, "left")
time.sleep(1.0)
print("after pickup click:", var177())
# hover empty space to see held image
hover(200, 60)
print("over empty space:", var177())

# Phase 2: hover kitten, click
kx, ky = actor_pos(4)
print("kitten at", kx, ky)
hover(kx, ky - 6)
print("over kitten:", var177())
click(kx, ky - 6, "left")
time.sleep(2.0)
print("after kitten click:", var177())
for i in range(15):
    s = client.state()
    inv = s.get("inventory")
    q = s.get("question")
    print("inv:", inv, "q:", bool(q))
    if inv and "carnival_tickets" in inv:
        break
    time.sleep(1.0)
