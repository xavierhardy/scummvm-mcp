import sys, time
from utils import MCP_HOST, McpClient, launch_scummvm, wait_for_mcp
BIN = "/Users/xhardy/Personal/llm/scummvm/myuser/scummvm/scummvm"
GAME = "/Users/xhardy/Personal/llm/scummvm/games/ManiacMansionDemo/Games/ManiacMansion"
PORT = 23457
def texts(r): return [m.get("text","") for m in (r or {}).get("messages",[])]
def rid(c): return (c.state().get("room") or {}).get("id")
def main():
    proc = launch_scummvm("maniac-c64", GAME, port=PORT, scummvm_binary=BIN, save_slot=1)
    try:
        c = wait_for_mcp(MCP_HOST, PORT, timeout=60)
        for v,t in (("walk_to","front_door"),("pull","door mat"),("pick_up","key"),
                    ("use","key"),):
            pass
        c.act("walk_to","front_door"); c.act("pull","door mat"); c.act("pick_up","key")
        c.act("use","key","front_door"); c.act("walk_to","front_door"); time.sleep(2)
        print("entered", rid(c), flush=True)
        # Interrogate + try each door by name
        for _ in range(4):
            print("open door:", texts(c.act("open","door")), flush=True)
            r = c.act("walk_to","door"); time.sleep(1.5)
            print("walk_to door -> room", rid(c), "msgs", texts(r), flush=True)
            if rid(c) != 10:
                print("  REACHED room", rid(c), "objs",
                      [o.get("name") for o in c.state().get("objects",[])], flush=True)
                break
    finally:
        proc.kill(); proc.wait(timeout=5)
sys.exit(main())
