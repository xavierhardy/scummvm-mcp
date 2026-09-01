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

#include "agos/mcp_names.h"

#include "common/array.h"
#include "common/str.h"
#include "common/util.h"

namespace AGOS {

namespace {

bool isSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isArticle(const Common::String &word) {
	return word == "a" || word == "an" || word == "the";
}

// The bar, in the order the engine keeps it. The engine's own table is in
// verb.cpp and is per-language; this is the agent-facing spelling of it, which
// is English whatever the game is speaking, because a tool argument is not
// prose.
struct VerbEntry {
	const char *name;
	// What else a caller might reasonably say for it. The first name is what
	// state() publishes; these are only accepted.
	const char *alias1;
	const char *alias2;
};

const VerbEntry kVerbs[] = {
	{ "walk_to",  "go_to",    "walk"     },
	{ "look_at",  "look",     "examine"  },
	{ "open",     nullptr,    nullptr    },
	{ "move",     "push",     "pull"     },
	{ "consume",  "eat",      "drink"    },
	{ "take",     "pick_up",  "get"      },
	{ "close",    "shut",     nullptr    },
	{ "use",      nullptr,    nullptr    },
	{ "talk_to",  "talk",     "speak_to" },
	{ "remove",   "take_off", nullptr    },
	{ "wear",     "put_on",   nullptr    },
	{ "give",     "give_to",  nullptr    }
};

} // End of anonymous namespace

Common::String agosObjectName(const Common::String &shownName) {
	Common::Array<Common::String> words;
	Common::String current;
	for (uint i = 0; i < shownName.size(); i++) {
		const char c = shownName[i];
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

Common::String agosDisambiguate(const Common::String &name, uint occurrence) {
	if (name.empty() || occurrence == 0)
		return name;
	return Common::String::format("%s_%u", name.c_str(), occurrence + 1);
}

int agosVerbIndex(const Common::String &verb) {
	for (int i = 0; i < ARRAYSIZE(kVerbs); i++) {
		if (verb == kVerbs[i].name)
			return i;
		if (kVerbs[i].alias1 != nullptr && verb == kVerbs[i].alias1)
			return i;
		if (kVerbs[i].alias2 != nullptr && verb == kVerbs[i].alias2)
			return i;
	}
	return -1;
}

Common::String agosVerbName(int index) {
	if (index < 0 || index >= ARRAYSIZE(kVerbs))
		return Common::String();
	return kVerbs[index].name;
}

int agosVerbCount() {
	return ARRAYSIZE(kVerbs);
}

} // End of namespace AGOS
