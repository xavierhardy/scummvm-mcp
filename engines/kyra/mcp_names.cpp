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

#include "kyra/mcp_names.h"

#include "common/array.h"
#include "common/str.h"
#include "common/util.h"

namespace Kyra {

namespace {

bool isSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isArticle(const Common::String &word) {
	return word == "a" || word == "an" || word == "the";
}

} // End of anonymous namespace

Common::String kyraItemName(const Common::String &printedName) {
	Common::Array<Common::String> words;
	Common::String current;
	for (uint i = 0; i < printedName.size(); i++) {
		const char c = printedName[i];
		if (isSpace(c) || c == '-' || c == '_') {
			if (!current.empty()) {
				words.push_back(current);
				current.clear();
			}
			continue;
		}
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			current += c;
		else if (c >= 'A' && c <= 'Z')
			current += (char)(c - 'A' + 'a');
	}
	if (!current.empty())
		words.push_back(current);

	uint first = 0;
	// Only a leading article goes, and only when something is left after it.
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

Common::String kyraDisambiguate(const Common::String &name, uint occurrence) {
	if (name.empty() || occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

Common::String kyraExitName(int direction) {
	switch (direction) {
	case 0:  return "exit_north";
	case 1:  return "exit_east";
	case 2:  return "exit_south";
	case 3:  return "exit_west";
	default: return Common::String();
	}
}

bool kyraIsEmptyItem(int itemId) {
	// -1 in the first game's signed table, 0xFFFF in the later games'
	// unsigned one; both mean the slot is empty.
	return itemId < 0 || itemId == 0xFFFF;
}

} // End of namespace Kyra
