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

namespace Gob {

Common::String mcpGobObjectName(const Common::String &label) {
	Common::String name = MCP::McpBridge::normalizeActionName(label);

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

	// Strip a leading article; exits keep their "to_..." form.
	const char *prefixes[] = {"a_", "an_", "the_"};
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
	Common::String name = MCP::McpBridge::normalizeActionName(label);
	return name.hasPrefix("to_");
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
