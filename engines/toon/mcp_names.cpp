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

#include "common/array.h"

#include "toon/mcp_names.h"

namespace Toon {

// Fold one raw character into the identifier alphabet. Returns 0 for a
// character that carries no meaning (punctuation, control codes).
static char foldChar(char c) {
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 'a';
	if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return c;
	if (c == ' ' || c == '-' || c == '_' || c == '\t')
		return '_';
	return 0;
}

// Lower-case, fold separators to '_', drop everything else, and collapse runs
// of '_' (including leading and trailing ones).
static Common::String foldToIdentifier(const Common::String &raw) {
	Common::String out;
	for (uint i = 0; i < raw.size(); i++) {
		char c = foldChar(raw[i]);
		if (c == 0)
			continue;
		if (c == '_' && (out.empty() || out[out.size() - 1] == '_'))
			continue;
		out += c;
	}
	while (!out.empty() && out[out.size() - 1] == '_')
		out.deleteLastChar();
	return out;
}

Common::String toonLabelToName(const Common::String &label) {
	return foldToIdentifier(label);
}

Common::String toonSceneName(const Common::String &roomFile) {
	uint start = 0;
	for (uint i = 0; i < roomFile.size(); i++)
		if (roomFile[i] == '/' || roomFile[i] == '\\')
			start = i + 1;

	Common::String base;
	for (uint i = start; i < roomFile.size(); i++) {
		if (roomFile[i] == '.')
			break;
		base += roomFile[i];
	}
	return foldToIdentifier(base);
}

Common::String toonFallbackName(const char *kind, int id) {
	return Common::String::format("%s_%d", kind ? kind : "thing", id);
}

Common::String toonDisambiguate(const Common::String &name, uint occurrence) {
	if (occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

Common::String toonTopicName(const Common::String &line, uint maxWords) {
	Common::String out;
	uint words = 0;
	bool inWord = false;
	for (uint i = 0; i < line.size() && words < maxWords; i++) {
		char c = foldChar(line[i]);
		if (c == 0)
			continue;
		if (c == '_') {
			if (inWord) {
				words++;
				inWord = false;
			}
			continue;
		}
		if (!inWord) {
			inWord = true;
			if (!out.empty())
				out += '_';
		}
		out += c;
	}
	return out;
}

// Words that open a description rather than name what it describes. Only
// dropped while they are still leading: "one stick of butter" loses "one",
// "a can of worms" keeps "worms".
static bool isLeadIn(const Common::String &word) {
	static const char *const kLeadIns[] = {
		"it", "its", "that", "this", "these", "those", "is", "are",
		"a", "an", "the", "some", "one", "just", "my", "your", "looks",
		"like", "seems", "to", "be", nullptr
	};
	for (uint i = 0; kLeadIns[i]; i++)
		if (word == kLeadIns[i])
			return true;
	return false;
}

Common::String toonItemName(const Common::String &description, uint maxWords) {
	// Split into folded words first, so the lead-in test sees whole words.
	Common::Array<Common::String> words;
	Common::String current;
	for (uint i = 0; i <= description.size(); i++) {
		char c = i < description.size() ? foldChar(description[i]) : '_';
		if (c == 0)
			continue;
		if (c == '_') {
			if (!current.empty()) {
				words.push_back(current);
				current.clear();
			}
			continue;
		}
		current += c;
	}

	uint first = 0;
	while (first < words.size() && isLeadIn(words[first]))
		first++;
	// Nothing but lead-ins: better a weak name than none at all.
	if (first >= words.size())
		first = 0;

	Common::String out;
	for (uint i = first; i < words.size() && i - first < maxWords; i++) {
		if (!out.empty())
			out += '_';
		out += words[i];
	}
	return out;
}

} // End of namespace Toon
