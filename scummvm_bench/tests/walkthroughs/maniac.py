"""Maniac Mansion C64 demo walkthrough (save slot 1) — the full collectathon."""

from scummvm_bench.backend import ScriptStep

from ._base import GAMES_DIR, Walkthrough, _door, _inv, _msgs, _pickup

# ---------------------------------------------------------------------------
# Maniac Mansion C64 demo (save slot 1) — the full 29-goal collectathon
# ---------------------------------------------------------------------------
#
# Reconciled against a live capture (see the maniac-c64-real-map memo). The demo
# is a true multi-kid game: ``switch_character`` teleports to that kid's own
# location (all start in room 1) and their own inventory. Dave collects every
# item and reaches kitchen/dining/pantry/living/library, then holds the right
# gargoyle (background object 342) in the hall so the second kid can slip through
# the handle-less basement door (36). Bernard does the basement (light switch +
# silver key), takes the radio tube Dave can't (Dave refuses it), and unlocks the
# pantry's far door (92) with the silver key out to the pool — the stopping room.
# Interior doors share the name "door" and are addressed by id; door 49 (kitchen
# -> dining) needs positioning first (walk_to, open, walk_to) once the actor has
# wandered to the fridge.

MANIAC = Walkthrough(
    game_id="maniac-c64",
    save_slot=1,
    initial_room=1,
    # Every reachable room, both light switches and every carriable item: the
    # full goal set in goals/maniac_mansion_c64_demo.py (== get_goal_set total).
    expected_goals=29,
    game_path_env="MANIAC_C64_PATH",
    game_path_default=str(GAMES_DIR / "ManiacMansionDemo/Games/ManiacMansion"),
    calls=[
        ("state", {}),  # 1  state_outside
        ("act", {"verb": "walk_to", "target1": "front_door"}),  # 2  approach
        ("act", {"verb": "pull", "target1": "door mat"}),  # 3  pull_door_mat
        ("act", {"verb": "pick_up", "target1": "key"}),  # 4  get_key
        ("act", {"verb": "use", "target1": "key", "target2": "front_door"}),  # 5
        ("act", {"verb": "walk_to", "target1": "front_door"}),  # 6 enter_mansion ->10
        # Dave: kitchen (35), flashlight + chainsaw, then the fridge contents.
        ("act", {"verb": "open", "target1": 35}),  # kitchen door
        ("act", {"verb": "walk_to", "target1": 35}),  # 8  reach_kitchen (->7)
        ("act", {"verb": "pick_up", "target1": "flashlight"}),  # 9  get_flashlight
        ("act", {"verb": "pick_up", "target1": "chainsaw"}),  # 10 get_chainsaw
        ("act", {"verb": "open", "target1": "refrigerator"}),  # 11 open_fridge
        ("act", {"verb": "pick_up", "target1": "old_batteries"}),  # 12 get_batteries
        ("act", {"verb": "pick_up", "target1": "can_of_pepsi"}),  # 13 get_pepsi
        ("act", {"verb": "pick_up", "target1": "cheese"}),  # 14 get_cheese
        ("act", {"verb": "pick_up", "target1": "lettuce"}),  # 15 get_lettuce
        ("act", {"verb": "pick_up", "target1": "broken_bottles_of_ketchup"}),  # ketchup
        # Dining (49): position, open, then enter (door 49 is finicky post-fridge).
        ("act", {"verb": "walk_to", "target1": 49}),  # position at the dining door
        ("act", {"verb": "open", "target1": 49}),
        ("act", {"verb": "walk_to", "target1": 49}),  # 18 reach_dining (->37)
        ("act", {"verb": "pick_up", "target1": "old_rotting_turkey"}),  # get_turkey
        ("act", {"verb": "pick_up", "target1": "week_old_roast"}),  # get_roast
        # Pantry (65).
        ("act", {"verb": "open", "target1": 65}),  # pantry door
        ("act", {"verb": "walk_to", "target1": 65}),  # 22 reach_pantry (->36)
        ("act", {"verb": "pick_up", "target1": "tentacle_chow"}),  # tentacle_chow
        ("act", {"verb": "pick_up", "target1": "canned_goods"}),  # canned_goods
        ("act", {"verb": "pick_up", "target1": "fruit_drinks"}),  # fruit_drinks
        ("act", {"verb": "pick_up", "target1": "glass_jar"}),  # glass_jar
        # Back-track pantry -> dining -> kitchen -> hall (far-side door ids).
        ("act", {"verb": "walk_to", "target1": 91}),  # (->37)
        ("act", {"verb": "walk_to", "target1": 62}),  # (->7)
        ("act", {"verb": "walk_to", "target1": 48}),  # (->10)
        # Living (37) then library (93): turn on the lamp, take the cassette.
        ("act", {"verb": "open", "target1": 37}),  # living-room door
        ("act", {"verb": "walk_to", "target1": 37}),  # 31 reach_living (->3)
        ("act", {"verb": "open", "target1": 93}),  # library door
        ("act", {"verb": "walk_to", "target1": 93}),  # 34 reach_library (->5)
        ("act", {"verb": "turn_on", "target1": "lamp"}),  # turn_on_library_lamp
        ("act", {"verb": "pull", "target1": "loose_panel"}),  # reveal the tape
        ("act", {"verb": "pick_up", "target1": "cassette_tape"}),  # cassette_tape
        # Return library -> living (102) -> hall (38) and hold the gargoyle.
        ("act", {"verb": "walk_to", "target1": 102}),  # (->3)
        ("act", {"verb": "walk_to", "target1": 38}),  # (->10)
        ("act", {"verb": "pull", "target1": 342}),  # hold the right gargoyle
        # Bernard slips through the handle-less basement door (36).
        ("switch_character", {"name": "bernard"}),  # -> room 1
        ("act", {"verb": "walk_to", "target1": "front_door"}),  # enter (->10)
        ("act", {"verb": "open", "target1": 36}),  # "There's no handle here!"
        ("act", {"verb": "walk_to", "target1": 36}),  # reach_basement (->8)
        ("act", {"verb": "turn_on", "target1": "light_switch"}),  # basement light
        ("act", {"verb": "pick_up", "target1": "silver_key"}),  # get_silver_key
        ("act", {"verb": "walk_to", "target1": 121}),  # basement stairs up (->10)
        # Bernard takes the radio tube Dave refused (living, room 3).
        ("act", {"verb": "open", "target1": 37}),  # living-room door
        ("act", {"verb": "walk_to", "target1": 37}),  # (->3)
        ("act", {"verb": "open", "target1": "old_fashion_radio"}),  # reveal the tube
        ("act", {"verb": "pick_up", "target1": "radio_tube"}),  # get_radio_tube
        ("act", {"verb": "walk_to", "target1": 38}),  # living -> hall (->10)
        # Hall -> kitchen -> dining -> pantry, then unlock the pool door (92).
        ("act", {"verb": "open", "target1": 35}),
        ("act", {"verb": "walk_to", "target1": 35}),  # (->7)
        ("act", {"verb": "open", "target1": 49}),
        ("act", {"verb": "walk_to", "target1": 49}),  # (->37)
        ("act", {"verb": "open", "target1": 65}),
        ("act", {"verb": "walk_to", "target1": 65}),  # (->36)
        ("act", {"verb": "unlock", "target1": 92, "target2": "silver_key"}),  # unlock
        ("act", {"verb": "walk_to", "target1": 92}),  # reach_pool (->6) STOP
    ],
    steps=[
        # Outside: first walk_to positions, second (after unlocking) enters; the
        # third walk_to front_door is Bernard's entry (replays the room-10 step).
        ScriptStep("act", {"verb": "walk_to", "target1": "front_door"}, {}),
        ScriptStep(
            "act", {"verb": "walk_to", "target1": "front_door"}, {"room_changed": 10}
        ),
        ScriptStep(
            "act", {"verb": "pull", "target1": "door mat"}, _door("door mat", 0, 8)
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "key"},
            {**_inv("key"), **_door("key", 0, 10)},
        ),
        ScriptStep(
            "act",
            {"verb": "use", "target1": "key", "target2": "front_door"},
            _door("front door", 4, 8),
        ),
        # Interior doors all read "door"; opening reports the state flip, walking
        # through changes room. Each id keeps its single destination (replayed on
        # the second pass), except door 49, whose first walk_to only positions.
        ScriptStep("act", {"verb": "open", "target1": 35}, _door("door")),
        ScriptStep("act", {"verb": "open", "target1": 49}, _door("door")),
        ScriptStep("act", {"verb": "open", "target1": 65}, _door("door")),
        ScriptStep("act", {"verb": "open", "target1": 37}, _door("door")),
        ScriptStep("act", {"verb": "open", "target1": 93}, _door("door")),
        ScriptStep(
            "act",
            {"verb": "open", "target1": 36},
            _msgs(("?", "There's no handle here!")),
        ),
        ScriptStep("act", {"verb": "walk_to", "target1": 35}, {"room_changed": 7}),
        ScriptStep("act", {"verb": "walk_to", "target1": 49}, {}),  # 1st: position
        ScriptStep("act", {"verb": "walk_to", "target1": 49}, {"room_changed": 37}),
        ScriptStep("act", {"verb": "walk_to", "target1": 65}, {"room_changed": 36}),
        ScriptStep("act", {"verb": "walk_to", "target1": 91}, {"room_changed": 37}),
        ScriptStep("act", {"verb": "walk_to", "target1": 62}, {"room_changed": 7}),
        ScriptStep("act", {"verb": "walk_to", "target1": 48}, {"room_changed": 10}),
        ScriptStep("act", {"verb": "walk_to", "target1": 37}, {"room_changed": 3}),
        ScriptStep("act", {"verb": "walk_to", "target1": 93}, {"room_changed": 5}),
        ScriptStep("act", {"verb": "walk_to", "target1": 102}, {"room_changed": 3}),
        ScriptStep("act", {"verb": "walk_to", "target1": 38}, {"room_changed": 10}),
        ScriptStep("act", {"verb": "walk_to", "target1": 36}, {"room_changed": 8}),
        ScriptStep("act", {"verb": "walk_to", "target1": 121}, {"room_changed": 10}),
        ScriptStep("act", {"verb": "walk_to", "target1": 92}, {"room_changed": 6}),
        # Kitchen pickups + the fridge (its contents are revealed by opening it).
        _pickup("flashlight"),
        _pickup("chainsaw"),
        ScriptStep(
            "act",
            {"verb": "open", "target1": "refrigerator"},
            _door("refrigerator", 0, 8),
        ),
        _pickup("old_batteries"),
        _pickup("can_of_pepsi"),
        _pickup("cheese"),
        _pickup("lettuce"),
        _pickup("broken_bottles_of_ketchup"),
        # Dining + pantry pickups.
        _pickup("old_rotting_turkey"),
        _pickup("week_old_roast"),
        _pickup("tentacle_chow"),
        _pickup("canned_goods"),
        _pickup("fruit_drinks"),
        _pickup("glass_jar"),
        # Library: the lamp is a call-goal; the cassette is behind the panel.
        ScriptStep("act", {"verb": "turn_on", "target1": "lamp"}, {}),
        ScriptStep(
            "act", {"verb": "pull", "target1": "loose_panel"}, _door("loose panel")
        ),
        _pickup("cassette_tape"),
        # Hold the gargoyle, switch to Bernard (-> his room 1).
        ScriptStep("act", {"verb": "pull", "target1": 342}, _door("door")),
        ScriptStep("switch_character", {"name": "bernard"}, {"room_changed": 1}),
        # Basement: the light switch is a call-goal; take the silver key.
        ScriptStep("act", {"verb": "turn_on", "target1": "light_switch"}, {}),
        _pickup("silver_key"),
        # Living: open the old radio for the tube Dave refused.
        ScriptStep(
            "act",
            {"verb": "open", "target1": "old_fashion_radio"},
            _door("old fashion radio"),
        ),
        _pickup("radio_tube"),
        # Unlock the pantry's far door with the silver key, step out to the pool.
        ScriptStep(
            "act",
            {"verb": "unlock", "target1": 92, "target2": "silver_key"},
            _door("door", 4, 8),
        ),
    ],
)
