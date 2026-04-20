# [ScummVM README](https://www.scummvm.org/) · [![Translation status](https://translations.scummvm.org/widgets/scummvm/-/scummvm/svg-badge.svg)](https://translations.scummvm.org/engage/scummvm/?utm_source=widget) [![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](CONTRIBUTING.md#pull-requests)

## MCP Server (AI Agent Integration)

This fork adds an **MCP (Model Context Protocol) server** that exposes SCUMM engine game state and controls to AI agents. It enables large language models to play classic LucasArts adventure games by observing game state and issuing actions via a standard TCP interface.

> **Compatibility:** Monkey Island 1 (including the demo) is the primary supported game and has been tested end-to-end. Other SCUMM engine games (MI2, Indiana Jones, Day of the Tentacle, Sam & Max, etc.) are partially supported — the core `state`/`act`/`answer` tools work but have not been thoroughly tested. Curse of Monkey Island (CMI) is experimental due to its different verb/UI system.

### Setup

Enable the server by adding `monkey_mcp=true` to the game's section in `scummvm.ini`, then launch with debug flags:

```ini
[monkey1]
monkey_mcp=true
```

```bash
scummvm --debugflags=monkey_mcp --debuglevel=1 monkey1
```

The server listens on TCP port **23456** and speaks the [MCP Streamable HTTP protocol (2025-03-26)](https://modelcontextprotocol.io/).

### MCP Tools

#### `state` — Read game state

Returns a snapshot of the current game state:

```json
{
  "room": 3,
  "room_name": "The Scumm Bar",
  "position": { "x": 320, "y": 140 },
  "verbs": ["open", "close", "give", "pick_up", "look_at", "talk_to", "walk_to", "push", "pull", "use"],
  "inventory": ["sword", "idol"],
  "objects": [
    { "id": 42, "name": "mug", "state": 1, "visible": true, "pathway": false, "compatible_verbs": ["look_at", "pick_up"] }
  ],
  "actors": ["guybrush", "pirate"],
  "messages": [{ "text": "You found it!", "actor": "guybrush" }],
  "question": {
    "choices": [
      { "id": 1, "label": "But I want to be a pirate!" },
      { "id": 2, "label": "Why not?" }
    ]
  }
}
```

`messages` contains dialog lines spoken since the last `state` call and is cleared on read. `question` is only present when a dialog choice is pending.

#### `act` — Perform an action

Executes a verb on an object and blocks (via SSE streaming) until the action completes, then returns what changed:

```json
{ "verb": "look_at", "object1": "mug" }
{ "verb": "use", "object1": "key", "object2": "door" }
{ "verb": "walk_to", "object1": "archway" }
{ "verb": "walk_to", "x": 200, "y": 150 }
```

```json
{
  "messages": [{ "text": "It's a wooden mug.", "actor": "guybrush" }],
  "inventory_added": ["mug"],
  "room_changed": 5,
  "position": { "x": 100, "y": 130 },
  "question": { "choices": [{ "id": 1, "label": "I want to be a pirate!" }] }
}
```

Verb names are case-insensitive and accept common aliases: `walk` → `walk_to`, `look` → `look_at`, `pick` / `pickup` → `pick_up`, `talk` → `talk_to`. Object names and IDs come from `state`. Prefer `object1` over `x`/`y` when the target is a named object or actor.

#### `answer` — Select a dialog choice

Selects a dialog option by 1-based index and blocks until the conversation completes:

```json
{ "id": 1 }
```

Returns the same state-change structure as `act`. Only valid when `state.question` is present.

### What It Can Do

- Read full game state: room, position, inventory, visible objects, actors, available verbs
- Execute any verb on any named object or actor (by name or numeric ID from `state`)
- Walk to named objects, actors, or explicit pixel coordinates
- Select dialog choices during conversations
- Block until an action fully completes — walk, cutscene, and dialog are all awaited
- Stream dialog lines as SSE notifications during long-running actions

### What It Cannot Do

- **Save or load games** — no save/restore tools are exposed
- **Read arbitrary game variables** — only the state fields listed above are available
- **See off-screen objects** — only objects present in the current room are listed
- **Control the camera** directly
- **Work reliably outside MI1** — other SCUMM games receive limited support; action completion detection and dialog heuristics are tuned for MI1
- **Work well with CMI** — Curse of Monkey Island's right-click UI means `verbs[]` is empty and verb resolution is unreliable
- **Guarantee completion detection** — unusual scripts may cause actions to time out (20 s hard limit) or close slightly early

### Architecture

The server runs inside the ScummVM game loop, implemented in `engines/scumm/monkey_mcp.cpp`. It accepts HTTP/MCP connections on port 23456, uses SSE to stream action progress, and integrates with the SCUMM engine's verb, object, actor, and script systems directly.

## About ScummVM

ScummVM allows you to play classic graphic point-and-click adventure games, text adventure games, and RPGs, as long as you already have the game data files. ScummVM replaces the executable files shipped with the games, which means you can now play your favorite games on all your favorite devices.

So how did ScummVM get its name? Many of the famous LucasArts adventure games, such as Maniac Mansion and the Monkey Island series, were created using a utility called SCUMM (Script Creation Utility for Maniac Mansion). The ‘VM’ in ScummVM stands for Virtual Machine.

While ScummVM was originally designed to run LucasArts’ SCUMM games, over time support has been added for many other games: see the full list [on our wiki](https://wiki.scummvm.org/index.php?title=Category:Supported_Games). Noteworthy titles include Broken Sword, Myst and Blade Runner, although there are countless other hidden gems to explore.

For more information, compatibility lists, details on donating, the
latest release, progress reports and more, please visit the ScummVM [home
page](https://www.scummvm.org/).

## Quickstart

For the impatient among you, here is how to get ScummVM running in five simple steps.

1. Download ScummVM from [our website](https://www.scummvm.org/downloads/) and install it.

2. Create a directory on your hard drive and copy the game datafiles from the original media to this directory. Repeat this for every game you want to play.

3. Start ScummVM, choose 'Add game', select the directory containing the game datafiles (do not try to select the datafiles themselves!) and press Choose.

4. The Game Options dialog opens to allow configuration of various settings for the game. These can be reconfigured at any time, but for now everything should be OK at the default settings.

5. Select the game you want to play in the list, and press Start. To play a game next time, skip to step 5, unless you want to add more games.

>
> Hint:
>
> To add multiple games in one go, click the small arrow on the 'Add game' button and choose 'Mass Add'. You are again asked to select a directory, only this time ScummVM will search through all subdirectories for supported games.



## Reporting a bug

To report a bug, go to the ScummVM [Issue Tracker](https://bugs.scummvm.org/) and log in with your GitHub account.

Please make sure the bug is reproducible, and still occurs in the latest git/[Daily build](https://buildbot.scummvm.org/#/dailybuilds) version. Also check the [compatibility list](https://www.scummvm.org/compatibility/) for that game, to ensure the issue is not already known. Please do not report bugs for games that are not listed as completable on the [Supported Games](https://wiki.scummvm.org/index.php?title=Category:Supported_Games) wiki page, or on the compatibility list. We already know those games have bugs!

Please include the following information in the bug report:

- ScummVM version (test the latest git/[Daily build](https://buildbot.scummvm.org/#/dailybuilds))
- Bug details, including instructions for how to reproduce the bug. If possible, include log files, screenshots, and any other relevant information.
- Game language
- Game version (for example, talkie or floppy)
- Platform and Compiler (for example, Win32, Linux or FreeBSD)
- An attached saved game, if possible.
- If this bug only occurred recently, include the last version without the bug, and the first version with the bug. That way we can fix it quicker by looking at the changes made.

Finally, please report each issue separately; do not file multiple issues on the same ticket. It is difficult to track the status of each individual bug when they aren't on their own tickets.

## Documentation

### User documentation

For everything you need to know about how to use ScummVM, see our [user documentation](https://docs.scummvm.org/).

### The ScummVM Wiki

[The wiki](https://wiki.scummvm.org/) is the place to go for information about every game supported by ScummVM. If you're a developer, there's also some very handy information in the Developer section.

### Changelog

Our extensive change log is available [here](NEWS.md).

## SAST Tools

[PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=github&utm_medium=organic&utm_campaign=open_source) - static analyzer for C, C++, C#, and Java code.

## Credits

A massive thank you to the entire team for making the ScummVM project possible. See the credits [here](AUTHORS)!

-----

> Good Luck and Happy Adventuring\!
> The ScummVM team.
> <https://www.scummvm.org/>
