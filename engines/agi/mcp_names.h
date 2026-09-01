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

#ifndef AGI_MCP_NAMES_H
#define AGI_MCP_NAMES_H

#include "common/str.h"

namespace Agi {

// Naming helpers for the AGI MCP bridge. Deliberately free of any engine
// dependency so the unit test links them without a running engine.
//
// AGI is not a pointer game and does not label what is on screen. What it
// names is two things, and both are words a person typed: the items in the
// OBJECT file ("a small chest of gold") and the parser's own vocabulary in
// WORDS.TOK ("chest", "gold", "open"). Everything an agent can refer to comes
// from one of those two lists, so these functions turn English written for a
// 1988 player into identifiers an agent can pass back.

// Identifier for an inventory item, from the name in the OBJECT file.
// Those names are written as prose - "A Small Chest Of Gold", "the key" -
// so the words are lower-cased and joined with underscores, and a leading
// article is dropped: "A Small Chest Of Gold" -> "small_chest_of_gold".
// Returns an empty string when nothing survives.
Common::String agiItemName(const Common::String &objectName);

// True when an OBJECT entry is a hole in the table rather than an item.
// AGI's object file is a fixed-length array and the unused slots are filled
// with "?" - which is a perfectly good thing to offer an agent as a target,
// right up until it tries to take it.
bool agiIsPlaceholderItem(const Common::String &objectName);

// Given a name and how many items before it already claimed that same name,
// the name to publish: the first keeps it, later ones are suffixed
// ("key", "key_2"). AGI games really do carry two objects of one name -
// King's Quest III has several identical spell components.
Common::String agiDisambiguate(const Common::String &name, uint occurrence);

// Fold a line the game printed into one line of text. AGI wraps its messages
// to the text window by hand, with newlines in the middle of sentences, so a
// message arrives as several rows that are one sentence: the rows are joined
// with a single space and runs of whitespace collapsed.
Common::String agiJoinMessage(const Common::String &raw);

// True when a printed line is the interpreter talking to the player rather
// than the game talking to the player - the prompt row, the score line, the
// "Press ENTER to continue" that the bridge's own skip answers. An agent
// reading those as game text is reading the furniture.
bool agiIsInterfaceLine(const Common::String &text);

} // End of namespace Agi

#endif
