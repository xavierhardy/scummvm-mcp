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

// ---------------------------------------------------------------------------
// McpBridgeTentacle — Day of the Tentacle
// ---------------------------------------------------------------------------

// Day of the Tentacle is a V6 game that kept the classic V3-V5 *text* verb bar
// (usesTextVerbBar()), and with it the standard verb-id layout shared with
// Monkey Island 2 and Fate of Atlantis. The V6 icon-verb table in mcp.cpp maps
// the same ids to Sam & Max's icon meanings, so spell the real ones out here.
// "Walk to" (11) is a live verb even though DOTT dropped it from the visible
// bar, so it is not in the game's own slot list.
const McpBridgeTentacle::TentacleVerb McpBridgeTentacle::kVerbs[] = {
	{2,  "open",    "open"},
	{3,  "close",   "close"},
	{4,  "give",    "give"},
	{5,  "push",    "push"},
	{6,  "pull",    "pull"},
	{7,  "use",     "use"},
	{8,  "look_at", "look at"},
	{9,  "pick_up", "pick up"},
	{10, "talk_to", "talk to"},
	{11, "walk_to", "walk to"},
	{0,  nullptr,   nullptr}
};

void McpBridgeTentacle::applyGameVerbs(Common::JSONArray &verbsArr,
                                       Common::Array<VerbInfo> &activeVerbs, bool questionPending) {
	if (questionPending)
		return;

	// The verb resource for "Look at" spells the double 'o' with a ligature glyph
	// from the game's own font, which decodes to a stray high-byte character.
	// Rewrite every recognised slot to its canonical name/label so agents see (and
	// can echo back) plain text. verbsArr and activeVerbs are built in lockstep by
	// toolState's text-verb pass, which is the only pass that runs for DOTT.
	bool listed[ARRAYSIZE(kVerbs)] = {};
	for (uint i = 0; i < activeVerbs.size(); ++i) {
		for (int k = 0; kVerbs[k].name; ++k) {
			if (activeVerbs[i].verbId != kVerbs[k].id)
				continue;
			listed[k] = true;
			if (activeVerbs[i].label == kVerbs[k].label)
				break;
			activeVerbs[i].name  = kVerbs[k].name;
			activeVerbs[i].label = kVerbs[k].label;
			if (i < verbsArr.size()) {
				delete verbsArr[i];
				verbsArr[i] = mcpJsonString(kVerbs[k].label);
			}
			break;
		}
	}

	// "Walk to" has no button on DOTT's bar (its slot stays hidden), but the verb
	// itself drives every exit. Expose it so pathway objects advertise it and
	// act(verb='walk to') resolves like it does in the other verb-bar games.
	for (int k = 0; kVerbs[k].name; ++k) {
		if (listed[k] || kVerbs[k].id != 11)
			continue;
		verbsArr.push_back(mcpJsonString(kVerbs[k].label));
		VerbInfo vi;
		vi.verbId = kVerbs[k].id;
		vi.name   = kVerbs[k].name;
		vi.label  = kVerbs[k].label;
		activeVerbs.push_back(vi);
	}
}

bool McpBridgeTentacle::resolveGameVerb(const Common::String &normalized, int &verbId) const {
	for (int k = 0; kVerbs[k].name; ++k) {
		if (normalized == kVerbs[k].name) {
			verbId = kVerbs[k].id;
			return true;
		}
	}
	return false;
}

} // End of namespace Scumm
