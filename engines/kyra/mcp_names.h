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

#ifndef KYRA_MCP_NAMES_H
#define KYRA_MCP_NAMES_H

#include "common/str.h"

namespace Kyra {

// Naming helpers for the Kyrandia MCP bridge. Deliberately free of any engine
// dependency so the unit test links them without a running engine.
//
// Kyrandia labels nothing on screen and holds no script names either: what it
// has is a table of item names, shipped as the strings the game writes into
// its own sentence line ("a golden ring", "the Kyragem"). Those are the only
// words the game itself uses for the things in it, so they are what an agent
// is given.

// Identifier for an item, from the name the game prints for it. Those names
// are written as prose with an article in front - "a golden ring" - so the
// article goes, the rest is lower-cased and joined with underscores, and
// punctuation is dropped: "a golden ring" -> "golden_ring". Returns an empty
// string when nothing survives.
Common::String kyraItemName(const Common::String &printedName);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("ring", "ring_2"). A Kyrandia scene really can hold two of one item.
Common::String kyraDisambiguate(const Common::String &name, uint occurrence);

// Name for one of a scene's four compass exits, which have no names at all -
// they are a direction and a doorway coordinate. `direction` is 0..3 for
// north, east, south, west.
Common::String kyraExitName(int direction);

// True when an item slot holds nothing. Kyrandia's scene and inventory tables
// are fixed-length arrays with -1 (and, in the later games, 0xFFFF) meaning
// "empty", and an agent offered an empty slot as a target is being offered
// nothing at all.
bool kyraIsEmptyItem(int itemId);

} // End of namespace Kyra

#endif
