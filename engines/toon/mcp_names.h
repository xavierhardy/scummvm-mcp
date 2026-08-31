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

#ifndef TOON_MCP_NAMES_H
#define TOON_MCP_NAMES_H

#include "common/str.h"

namespace Toon {

// Naming helpers for the Toon MCP bridge. Deliberately free of any engine
// dependency so the unit test links them without a running engine.
//
// Every name an agent sees comes from the game's own data: the line the game
// writes along the bottom of the screen when the player points at something,
// the way out's destination as the game names it, and the file a scene's data
// lives in. These functions only fold those raw strings into stable
// identifiers, and supply a fallback for the things the game leaves unnamed.

// Identifier for a thing on screen, from the line the game writes for it.
// Lower-cased, spaces and dashes folded to underscores, everything else
// dropped ("Bottomless Bag" -> "bottomless_bag", "Drew's hat!" ->
// "drews_hat"). Returns an empty string when nothing survives.
Common::String toonLabelToName(const Common::String &label);

// Identifier for a scene, from the base name of the file its data lives in
// ("TAVERN" -> "tavern", "BOOKSHOP.PAK" -> "bookshop"). Empty in, empty out.
Common::String toonSceneName(const Common::String &roomFile);

// Name for something the game never labels, so it can still be targeted.
// `kind` is the sort of thing it is ("object", "exit", "item", "topic").
Common::String toonFallbackName(const char *kind, int id);

// Given a name and how many things before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("door", "door_2", "door_3").
Common::String toonDisambiguate(const Common::String &name, uint occurrence);

// A conversation option carries no label of its own — what identifies it is
// the line the player character will say when it is picked. Shorten that line
// into something an agent can echo back: the first few words, folded into an
// identifier. Returns an empty string when nothing survives.
Common::String toonTopicName(const Common::String &line, uint maxWords = 5);

// An item in the bag carries no label either: the only thing the game ever
// says about it is the line the player character speaks when asked about it
// ("A plunger.", "One stick of Marge's butter."). Turn that into a name by
// dropping the words that open a sentence rather than name a thing ("it's",
// "a", "the", "one", ...) and keeping the first few that are left
// ("plunger", "stick_of_marges_butter"). Empty in, empty out.
Common::String toonItemName(const Common::String &description, uint maxWords = 4);

} // End of namespace Toon

#endif
