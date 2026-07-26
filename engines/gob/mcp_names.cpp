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

#include "gob/mcp_names.h"

#include "engines/mcp_bridge.h"

#include "common/ustr.h"

namespace Gob {

// The ASCII stand-in for the Latin-1 supplement letters, U+00C0 to U+00FF.
// Nothing else is folded: the rest of the range (and everything above it) is
// left to the [a-z0-9_] filter in mcpGobObjectName().
static const char *const kLatin1Folded[64] = {
	"A", "A", "A", "A", "A", "A", "AE", "C",   // C0-C7
	"E", "E", "E", "E", "I", "I", "I", "I",    // C8-CF
	"D", "N", "O", "O", "O", "O", "O", "",     // D0-D7 (D7 = multiplication sign)
	"O", "U", "U", "U", "U", "Y", "TH", "ss",  // D8-DF
	"a", "a", "a", "a", "a", "a", "ae", "c",   // E0-E7
	"e", "e", "e", "e", "i", "i", "i", "i",    // E8-EF
	"d", "n", "o", "o", "o", "o", "o", "",     // F0-F7 (F7 = division sign)
	"o", "u", "u", "u", "u", "y", "th", "y"    // F8-FF
};

Common::String mcpGobFoldAccents(const Common::String &utf8) {
	Common::U32String wide = utf8.decode(Common::kUtf8);
	Common::String out;
	for (uint i = 0; i < wide.size(); i++) {
		uint32 cp = wide[i];
		if (cp < 0x80) {
			out += (char)cp;
		} else if (cp >= 0xC0 && cp <= 0xFF) {
			out += kLatin1Folded[cp - 0xC0];
		} else if (cp == 0x152 || cp == 0x153) {  // OE / oe ligature
			out += cp == 0x152 ? "OE" : "oe";
		} else {
			// Anything else has no ASCII stand-in; drop it like the filter
			// in mcpGobObjectName() would.
			out += ' ';
		}
	}
	return out;
}

Common::String mcpGobTextToUtf8(const Common::String &text) {
	// Pure ASCII (every English line, and most of the localised ones) is already
	// UTF-8; only pay for the conversion when it is not.
	for (uint i = 0; i < text.size(); i++) {
		if ((byte)text[i] >= 0x80)
			return text.decode(Common::kDos850).encode(Common::kUtf8);
	}
	return text;
}

Common::String mcpGobObjectName(const Common::String &label) {
	Common::String name = MCP::McpBridge::normalizeActionName(mcpGobFoldAccents(label));

	// Keep the identifier plain: [a-z0-9_] only.
	Common::String plain;
	for (uint i = 0; i < name.size(); i++) {
		char c = name[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
			plain += c;
	}
	// Collapse runs of '_' left by stripped characters, and trim the ends.
	Common::String collapsed;
	for (uint i = 0; i < plain.size(); i++) {
		if (plain[i] == '_' && (collapsed.empty() || collapsed.lastChar() == '_'))
			continue;
		collapsed += plain[i];
	}
	while (collapsed.size() && collapsed.lastChar() == '_')
		collapsed.deleteLastChar();

	// Strip a leading article; exits keep their "to_..."/"vers_..." form.
	const char *prefixes[] = {"a_", "an_", "the_",          // English
	                          "un_", "une_", "le_", "la_", "les_"};  // French
	for (uint i = 0; i < ARRAYSIZE(prefixes); i++) {
		if (collapsed.hasPrefix(prefixes[i]) &&
		    collapsed.size() > strlen(prefixes[i])) {
			collapsed = Common::String(collapsed.c_str() + strlen(prefixes[i]));
			break;
		}
	}
	return collapsed;
}

bool mcpGobIsExitLabel(const Common::String &label) {
	Common::String name = MCP::McpBridge::normalizeActionName(mcpGobFoldAccents(label));
	// The exits are the labels that name where they lead: "TO STAIRS STREET" in
	// English, "VERS LA RUE DE L'ESCALIER" in French.
	return name.hasPrefix("to_") || name.hasPrefix("vers_");
}

Common::String mcpGobHotspotFallbackName(uint16 id) {
	return Common::String::format("hotspot_%u", id);
}

int mcpGobParseHotspotTarget(const Common::String &normalized) {
	const char *s = normalized.c_str();
	if (normalized.hasPrefix("hotspot_"))
		s = normalized.c_str() + 8;
	if (!*s)
		return -1;
	int value = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		value = value * 10 + (*s - '0');
	}
	return value;
}

Common::String mcpGobScreenName(const Common::String &totFile) {
	Common::String name = totFile;
	name.toLowercase();
	if (name.hasSuffix(".tot"))
		name.erase(name.size() - 4);
	return name;
}

int mcpGobScreenId(const Common::String &totFile) {
	Common::String name = mcpGobScreenName(totFile);
	uint end = name.size();
	uint start = end;
	while (start > 0 && name[start - 1] >= '0' && name[start - 1] <= '9')
		start--;
	if (start == end)
		return 0;
	int value = 0;
	for (uint i = start; i < end; i++)
		value = value * 10 + (name[i] - '0');
	return value;
}

} // End of namespace Gob
