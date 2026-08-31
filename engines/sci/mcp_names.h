/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */


#ifndef SCI_MCP_NAMES_H
#define SCI_MCP_NAMES_H

#include "common/str.h"

namespace Sci {

// Naming helpers for the SCI MCP bridge. Deliberately free of any engine
// dependency so the unit test links them without a running engine.
//
// SCI is unlike the pointer games the other bridges cover: it does not label
// what the player points at. What it has instead is a script-level name on
// every object — the name the game's own author typed — carried in the object
// header and readable straight out of the segment manager. So these functions
// fold an author's identifier into an agent's, rather than folding a painted
// label into one.

// Identifier for a thing on screen, from the name its script carries.
// SCI names are camelCase or run together ("gateDoor", "hansGate",
// "theShovel"), so the camel humps become underscores and everything else is
// lower-cased and folded: "gateDoor" -> "gate_door", "GK1Door2" ->
// "gk1_door2". A leading "the" is dropped, because a script that says
// "theShovel" and one that says "shovel" mean the same thing and an agent
// should not have to know which one this game's author preferred. Returns an
// empty string when nothing survives.
Common::String sciObjectName(const Common::String &scriptName);

// Name for something the script leaves unnamed, so it can still be targeted.
// `kind` is the sort of thing it is ("object", "item", "exit", "topic").
Common::String sciFallbackName(const char *kind, int id);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("door", "door_2", "door_3").
Common::String sciDisambiguate(const Common::String &name, uint occurrence);

// Identifier for a room, from the name of the room object the game is in
// ("hansGate" -> "hans_gate"). SCI rooms are numbered, not named, so this is
// only ever a companion to the number, never a replacement for it.
Common::String sciRoomName(const Common::String &roomObjectName);

// True when a script name is one of the engine's own bookkeeping objects
// rather than something in the room an agent could act on. SCI scripts put
// their timers, movers, sound handles and scaling helpers in the same cast
// list as the actors, and an agent offered "aMover" as a thing to look at is
// being offered a distraction.
bool sciIsInternalName(const Common::String &scriptName);

} // End of namespace Sci

#endif
