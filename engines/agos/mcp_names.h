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

#ifndef AGOS_MCP_NAMES_H
#define AGOS_MCP_NAMES_H

#include "common/str.h"

namespace AGOS {

// Naming helpers for the AGOS MCP bridge. Deliberately free of any engine
// dependency so the unit test links them without a running engine.
//
// AGOS is the friendliest of these engines to name things from: every clickable
// thing on screen is a hit area with an item behind it, and the item carries
// the name the game writes along the bottom of the screen when the pointer
// rests on it ("a large wooden door"). So these fold a label a player reads
// into an identifier an agent can pass back.

// Identifier for a thing, from the name the game shows for it. Those names are
// prose with an article in front, so the article goes, the rest is lower-cased
// and joined with underscores, and punctuation is dropped: "a large wooden
// door" -> "large_wooden_door". Returns an empty string when nothing survives.
Common::String agosObjectName(const Common::String &shownName);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("door", "door_2"). Simon's rooms really do hold two identical things.
Common::String agosDisambiguate(const Common::String &name, uint occurrence);

// The engine's verb, from the word an agent used. AGOS has a fixed bar of
// twelve verbs and they are the same twelve in every game it runs; this maps
// the names a tool caller would reach for onto that bar, so "look_at",
// "examine" and "look" all find "Look at". Returns -1 for a word the bar has
// no button for.
int agosVerbIndex(const Common::String &verb);

// The name of the verb at `index` on the bar, as an agent should say it, or an
// empty string when there is no such verb.
Common::String agosVerbName(int index);

// How many verbs the bar has. The bridge publishes all of them in state(), so
// an agent never has to guess which words this game takes.
int agosVerbCount();

} // End of namespace AGOS

#endif
