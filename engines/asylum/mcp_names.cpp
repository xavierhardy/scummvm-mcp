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

#include "asylum/mcp_names.h"

#include "common/array.h"
#include "common/str.h"
#include "common/util.h"

namespace Asylum {

namespace {

bool isSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // End of anonymous namespace

Common::String asylumObjectName(const Common::String &dataName) {
	Common::Array<Common::String> words;
	Common::String current;
	for (uint i = 0; i < dataName.size(); i++) {
		const char c = dataName[i];
		if (isSpace(c) || c == '-' || c == '_' || c == '.') {
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

	Common::String out;
	for (uint i = 0; i < words.size(); i++) {
		if (!out.empty())
			out += '_';
		out += words[i];
	}
	return out;
}

Common::String asylumDisambiguate(const Common::String &name, uint occurrence) {
	if (name.empty() || occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

bool asylumIsPlaceholderName(const Common::String &dataName) {
	const Common::String folded = asylumObjectName(dataName);
	if (folded.empty())
		return true;
	// A name that is only digits is a slot number rather than a name.
	bool allDigits = true;
	for (uint i = 0; i < folded.size(); i++) {
		if (folded[i] < '0' || folded[i] > '9')
			allDigits = false;
	}
	if (allDigits)
		return true;
	// The fillers the editor leaves behind.
	static const char *const kFiller[] = { "x", "xx", "xxx", "none", "null", "unused", "temp" };
	for (uint i = 0; i < ARRAYSIZE(kFiller); i++) {
		if (folded == kFiller[i])
			return true;
	}
	return false;
}

} // End of namespace Asylum
