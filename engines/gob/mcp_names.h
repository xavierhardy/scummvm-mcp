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

#ifndef GOB_MCP_NAMES_H
#define GOB_MCP_NAMES_H

#include "common/str.h"

// Naming helpers for the Gob MCP bridge. Deliberately engine-free (no Engine,
// no GobEngine) so the CxxTest unit suite links them without a running engine.

namespace Gob {

// Decode a string the game drew into UTF-8. Woodruff stores its text in the DOS
// OEM code page (CP850) even in its Windows release, so the localised versions
// draw accented letters as single high bytes ("d\x82tritus"); left alone they
// are not valid UTF-8 and every accented character is lost on the wire.
Common::String mcpGobTextToUtf8(const Common::String &text);

// Fold the accented Latin letters of a UTF-8 string onto their ASCII base
// letter ("détritus" -> "detritus"). The localised releases draw accented text,
// which would otherwise be dropped from the identifiers altogether.
Common::String mcpGobFoldAccents(const Common::String &utf8);

// Turn a hover label the game drew ("A trash heap", "Un recueil de détritus")
// into a stable target identifier: accents folded, then
// MCP::McpBridge::normalizeActionName() plus a leading article ("a"/"an"/"the",
// "un"/"une"/"le"/"la"/"les") stripped and everything outside [a-z0-9_]
// dropped. Empty when nothing survives.
Common::String mcpGobObjectName(const Common::String &label);

// True when a hover label reads like a screen exit ("TO THE ...", "VERS LA ...").
bool mcpGobIsExitLabel(const Common::String &label);

// The fallback identifier for an unnamed hotspot.
Common::String mcpGobHotspotFallbackName(uint16 id);

// Parse a target of the form "hotspot_<id>" or a bare "<id>". Returns -1 when
// the string is neither.
int mcpGobParseHotspotTarget(const Common::String &normalized);

// Room/screen name from a TOT file name: "EMAP2002.TOT" -> "emap2002".
Common::String mcpGobScreenName(const Common::String &totFile);

// Numeric room/screen id from a TOT file name: the trailing number when there
// is one ("EMAP2002.TOT" -> 2002), 0 otherwise ("MENU.tot").
int mcpGobScreenId(const Common::String &totFile);

} // End of namespace Gob

#endif
