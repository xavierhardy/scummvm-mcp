/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "scumm/mcp_subclasses.h"
#include "scumm/scumm.h"

namespace Scumm {

using Networking::mcpJsonString;

// Game/version-specific MCP bridge logic for the V6 SCUMM family (Sam & Max).
// Migrated out of mcp.cpp via the ScummMcpBridge virtual hooks.
//
// NOTE: Sam & Max's right-click context-cursor state machine (the toolAct
// cursor-setup branches and the per-frame pumpStream driver, plus its dialog-
// choice handling that shares the V7 blast-text machinery) still lives in the
// base bridge. Extracting it needs a richer act-dispatch contract than the
// current dispatchGameAct bool and the shared dialog-choice plumbing to be
// factored first; it is left for a follow-up.

// ---------------------------------------------------------------------------
// McpBridgeSamnMax — Sam & Max Hit the Road
// ---------------------------------------------------------------------------

void McpBridgeSamnMax::applyGameVerbs(Common::JSONArray &verbsArr,
                                      Common::Array<VerbInfo> &activeVerbs, bool questionPending) {
	// Sam & Max (V6) does not populate the classic text verb slots; expose a
	// stable MCP verb set even when _verbs[] is empty. Its icon-verb ids differ
	// from the common V6 layout (eye = look at = 15, hand = pick up = 14).
	if (questionPending || !activeVerbs.empty())
		return;
	static const struct { int id; const char *name; const char *label; } kSamnMaxFallback[] = {
		{13, "walk_to", "walk to"},
		{15, "look_at", "look at"},
		{11, "use", "use"},
		{6,  "talk_to", "talk to"},
		{14, "pick_up", "pick up"},
		{0, nullptr, nullptr}
	};
	for (int i = 0; kSamnMaxFallback[i].name; ++i) {
		verbsArr.push_back(mcpJsonString(kSamnMaxFallback[i].label));
		VerbInfo vi;
		vi.verbId = kSamnMaxFallback[i].id;
		vi.name = kSamnMaxFallback[i].name;
		vi.label = kSamnMaxFallback[i].label;
		activeVerbs.push_back(vi);
	}
}

} // End of namespace Scumm
