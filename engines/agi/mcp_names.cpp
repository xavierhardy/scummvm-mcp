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

#include "agi/mcp_names.h"

#include "common/array.h"
#include "common/str.h"
#include "common/util.h"

namespace Agi {

namespace {

bool isSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// The words an item name may start with that say nothing about the item.
bool isArticle(const Common::String &word) {
	return word == "a" || word == "an" || word == "the";
}

} // End of anonymous namespace

Common::String agiItemName(const Common::String &objectName) {
	Common::Array<Common::String> words;
	Common::String current;
	for (uint i = 0; i < objectName.size(); i++) {
		const char c = objectName[i];
		if (isSpace(c) || c == '-' || c == '_') {
			if (!current.empty()) {
				words.push_back(current);
				current.clear();
			}
			continue;
		}
		// Punctuation is dropped rather than kept: an agent typing a name back
		// should not have to reproduce an apostrophe.
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			current += c;
		} else if (c >= 'A' && c <= 'Z') {
			current += (char)(c - 'A' + 'a');
		}
	}
	if (!current.empty())
		words.push_back(current);

	uint first = 0;
	// Only a *leading* article goes: "chest of gold" keeps its "of", and an
	// item actually called "the" (there is none, but the table has holes in
	// it) keeps its only word rather than becoming nothing.
	if (words.size() > 1 && isArticle(words[0]))
		first = 1;

	Common::String out;
	for (uint i = first; i < words.size(); i++) {
		if (!out.empty())
			out += '_';
		out += words[i];
	}
	return out;
}

bool agiIsPlaceholderItem(const Common::String &objectName) {
	// The unused slots are "?" in every game that has them. Anything that
	// folds away to nothing is equally unusable as a target.
	Common::String trimmed;
	for (uint i = 0; i < objectName.size(); i++) {
		if (!isSpace(objectName[i]))
			trimmed += objectName[i];
	}
	return trimmed.empty() || trimmed == "?";
}

Common::String agiDisambiguate(const Common::String &name, uint occurrence) {
	if (name.empty())
		return name;
	if (occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

Common::String agiJoinMessage(const Common::String &raw) {
	Common::String out;
	bool pendingSpace = false;
	for (uint i = 0; i < raw.size(); i++) {
		const char c = raw[i];
		if (isSpace(c)) {
			// A run of any whitespace - including the hand-wrapped newlines
			// in the middle of a sentence - is one space, and only once
			// something has been written, so leading space never appears.
			pendingSpace = !out.empty();
			continue;
		}
		if (pendingSpace) {
			out += ' ';
			pendingSpace = false;
		}
		out += c;
	}
	return out;
}

bool agiIsInterfaceLine(const Common::String &text) {
	const Common::String joined = agiJoinMessage(text);
	if (joined.empty())
		return true;
	Common::String lower;
	for (uint i = 0; i < joined.size(); i++) {
		const char c = joined[i];
		lower += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}
	// The prompt row is what the player types into, and it arrives as text
	// like everything else. So do the status line and the interpreter's own
	// "press a key" - none of them is the game saying anything.
	if (lower.hasPrefix(">") || lower.hasPrefix("]"))
		return true;
	// The clock. King's Quest III draws a running "0:00:07" every second, and
	// an agent reading those as lines the game said is reading a stopwatch:
	// anything made only of digits, colons and dots is furniture.
	bool digitsAndSeparators = true;
	bool sawDigit = false;
	for (uint i = 0; i < lower.size(); i++) {
		const char c = lower[i];
		if (c >= '0' && c <= '9')
			sawDigit = true;
		else if (c != ':' && c != '.' && c != ' ')
			digitsAndSeparators = false;
	}
	if (sawDigit && digitsAndSeparators)
		return true;
	static const char *const kFurniture[] = {
		"score:", "sound:", "press enter to continue", "press a key to continue",
		"press esc to", "type space", "restore game", "save game"
	};
	for (uint i = 0; i < ARRAYSIZE(kFurniture); i++) {
		if (lower.contains(kFurniture[i]))
			return true;
	}
	return false;
}

} // End of namespace Agi
