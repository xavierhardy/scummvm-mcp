"""Goal set for the Maniac Mansion C64 "hands-on" demo, save slot 1 (outside).

The demo has no win state (decompiling the C64 disk: the boot script hands the
player control in the front yard with no timer or end-script; only the
*self-running* demo, script 6, ever calls ``restart()``). "Completing" the
hands-on demo therefore means visiting every reachable room, turning on the
lights in the dark ones, and collecting every item the demo lets you carry.
Reconciled against a live MCP playthrough (and ``test/mcp/test_maniac_c64.py``).

Route: outside (room 1) pull the door mat to reveal + take the KEY, use it on the
front door, enter the hall (room 10) -> kitchen (7): FLASHLIGHT, CHAINSAW, open
the fridge for OLD BATTERIES / CAN OF PEPSI / CHEESE / LETTUCE / KETCHUP -> dining
(37): OLD ROTTING TURKEY, WEEK-OLD ROAST -> pantry (36): TENTACLE CHOW, CANNED
GOODS, FRUIT DRINKS, GLASS JAR -> living (3): open the old radio and (as BERNARD;
Dave refuses) take the RADIO TUBE -> library (5): turn on the lamp, pull the loose
panel for the CASSETTE TAPE -> the basement door has no handle: one kid holds the
right gargoyle (object 342) while another slips through to the basement (room 8),
turn on the light switch in the dark reactor room and take the SILVER KEY -> the
silver key unlocks the pantry's far door out to the swimming pool (room 6), the
deepest reachable room.

Parent-gated reveals (mirror the engine's findObject(): the MCP hides the child
until the parent reaches the revealing state, so it cannot be grabbed early --
verified in test_maniac_c64.py): the KEY exists only once the mat is pulled; the
fridge contents only once the refrigerator is opened.

Lights are toggled via the V0 ``lights()`` opcode, not an object state, so the two
"turn on" goals are call-based. The library lamp / basement light switch / the
gargoyles are V0 background objects whose ids exceed 255, so they are reachable by
name through ``act`` (toolAct widens the V0 id ceiling) though not listed in state.

NOT carriable (confirmed): the pool RADIO (across the never-drainable pool) and
the pantry BOTTLE OF DEVELOPER ("Whoops!" -- it spills and is removed). RADIO TUBE
needs Bernard; every other item can be Dave's. Upstairs is blocked ("I can't go up
until you buy the game") and the back-yard pool gate stays locked.

Ids by number where names collide: right gargoyle 342 (left 344); pantry->pool
door 92. Interior doors all read "door" and are walked by id (kitchen 35, dining
49, pantry 65; 48/62/91 back-track; living 37, library 93).
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    on_call,
    on_inventory_added,
    on_object_changed,
    on_room_changed,
)


def _goal(goal_id, label, predicate, kind="result", stopping=False) -> Goal:
    return Goal(goal_id, label, predicate, stopping=stopping, kind=kind)


# Reachable rooms (room id, goal id, label). The hall is covered by enter_mansion.
_ROOMS = [
    (7, "reach_kitchen", "Reach the kitchen"),
    (37, "reach_dining", "Reach the dining room"),
    (36, "reach_pantry", "Reach the pantry"),
    (3, "reach_living", "Reach the living room"),
    (5, "reach_library", "Reach the library"),
    (8, "reach_basement", "Hold the gargoyle and slip through the handleless basement door"),
    (6, "reach_pool", "Unlock the pantry door with the silver key and reach the pool"),
]

# Every item the demo lets a kid carry (goal id, label, inventory name).
_ITEMS = [
    ("get_key", "Take the key (revealed by pulling the mat)", "key"),
    ("get_flashlight", "Take the flashlight (kitchen)", "flashlight"),
    ("get_chainsaw", "Take the chainsaw (kitchen)", "chainsaw"),
    ("get_batteries", "Take the old batteries (inside the fridge)", "old_batteries"),
    ("get_pepsi", "Take the can of pepsi (inside the fridge)", "can_of_pepsi"),
    ("get_cheese", "Take the cheese (inside the fridge)", "cheese"),
    ("get_lettuce", "Take the lettuce (inside the fridge)", "lettuce"),
    ("get_ketchup", "Take the broken bottles of ketchup (inside the fridge)", "broken_bottles_of_ketchup"),
    ("get_turkey", "Take the old rotting turkey (dining room)", "old_rotting_turkey"),
    ("get_roast", "Take the week-old roast (dining room)", "week_old_roast"),
    ("get_tentacle_chow", "Take the tentacle chow (pantry)", "tentacle_chow"),
    ("get_canned_goods", "Take the canned goods (pantry)", "canned_goods"),
    ("get_fruit_drinks", "Take the fruit drinks (pantry)", "fruit_drinks"),
    ("get_glass_jar", "Take the glass jar (pantry)", "glass_jar"),
    ("get_radio_tube", "Take the radio tube (open the old radio; Bernard only)", "radio_tube"),
    ("get_cassette_tape", "Take the cassette tape (behind the library loose panel)", "cassette_tape"),
    ("get_silver_key", "Take the silver key (basement, by the fuse box)", "silver_key"),
]


def _build() -> dict:
    goals = [
        _goal("pull_door_mat", "Pull the door mat (reveals the key)",
              on_object_changed("door mat")),
        _goal("enter_mansion", "Unlock the front door with the key and go inside",
              on_room_changed(10)),
        _goal("open_fridge", "Open the refrigerator (reveals its contents)",
              on_object_changed("refrigerator")),
    ]
    goals += [
        _goal(gid, label, on_room_changed(rid), stopping=(rid == 6))
        for rid, gid, label in _ROOMS
    ]
    goals += [
        _goal("turn_on_library_lamp", "Turn on the library lamp",
              on_call("act", verb="turn_on", target1="lamp"), kind="call"),
        _goal("turn_on_basement_light", "Turn on the light switch in the dark basement",
              on_call("act", verb="turn_on", target1="light_switch"), kind="call"),
    ]
    goals += [_goal(gid, label, on_inventory_added(item)) for gid, label, item in _ITEMS]
    return {g.goal_id: g for g in goals}


MANIAC_C64_DEMO_GOALSET = GoalSet(
    game_id="maniac-c64",
    save_slot=1,
    goals=_build(),
)
