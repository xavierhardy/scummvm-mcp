/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SCUMM_MCP_SUBCLASSES_H
#define SCUMM_MCP_SUBCLASSES_H

#include "scumm/mcp.h"

namespace Scumm {

// The MCP bridge class hierarchy. The base ScummMcpBridge (mcp.h) holds the
// engine-agnostic transport, the streaming/state machinery, and the shared
// tool implementations. Each SCUMM engine version has an intermediate base
// class carrying that version's behaviour; individual games that need extra
// logic derive a leaf class from their version base. ScummMcpBridge::create()
// picks the right class for the running game, falling back to the version base
// for games without a dedicated leaf.
//
// Game-specific behaviour is migrated out of the base into these classes
// incrementally via the protected virtual hooks declared on ScummMcpBridge.

// --- V0–V2: early SCUMM (Maniac Mansion / Zak C64-era) ---------------------
class McpBridgeV0 : public ScummMcpBridge {
public:
	explicit McpBridgeV0(ScummEngine *vm) : ScummMcpBridge(vm) {}
};

class McpBridgeManiac : public McpBridgeV0 {
public:
	explicit McpBridgeManiac(ScummEngine *vm) : McpBridgeV0(vm) {}

protected:
	void registerGameTools() override;
	Common::JSONValue *dispatchGameTool(const Common::String &name,
	                                    const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentState(Common::JSONObject &out) override;
	void pumpStreamGame() override;
	void resetGameStream() override;
	bool gameStreamBusy() const override;

private:
	// The playable kids. Slot is the F1-F3 index V0's switchActor() takes; the
	// actor id comes from VAR(97 + slot). The V1/V2 ports switch kids in-game via
	// the "New Kid" verb, but the same ego/kid vars drive the switch.
	struct ManiacKid {
		int slot;
		int actorId;
		Common::String name;
	};
	void collectManiacKids(Common::Array<ManiacKid> &out) const;
	bool toolSwitchCharacter(const Common::JSONValue &args, Common::String &errorOut);
	bool toolDial(const Common::JSONValue &args, Common::String &errorOut);

	// Phone dial pad: queued button object ids to press one at a time, the verb
	// id used to press them, and the frame of the last press.
	Common::Array<int> _ssePendingDialObjs;
	int _sseDialVerbId = 0;
	uint32 _sseLastDialFedFrame = 0;
};

// --- V3–V5: classic verb-bar SCUMM -----------------------------------------
// Hosts the shared Loom-note and Indy3-fight helpers so the Loom, Indy3 and
// (mixed-demo) Passport leaves can all reuse them under single inheritance.
class McpBridgeClassic : public ScummMcpBridge {
public:
	explicit McpBridgeClassic(ScummEngine *vm) : ScummMcpBridge(vm) {}

protected:
	// Shared helpers reused by the Loom / Indy3 / Passport leaves.
	bool indy3FightActive() const;          // Indy3 fight-HUD var heuristic
	bool loomSectionByVerbLabels() const;   // Loom-in-Passport verb-bar detection
	void applyLoomVerbs(Common::JSONArray &verbsArr, Common::Array<VerbInfo> &activeVerbs,
	                    bool questionPending);
	void addIndy3FightHud(Common::JSONObject &out) const;
	void registerPlayNoteTool();
	Common::JSONValue *dispatchPlayNote(const Common::String &name, const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled);
	bool toolPlayNote(const Common::JSONValue &args, Common::String &errorOut);

	// Loom note watcher + deferred note/click feed, active only inside a Loom
	// section (full Loom, or the Loom mini-game in Passport). Surfaces var(259)
	// note transitions as MCP notifications and paces play_note's queued keys.
	void pumpStreamGame() override;
};

class McpBridgeLoom : public McpBridgeClassic {
public:
	explicit McpBridgeLoom(ScummEngine *vm) : McpBridgeClassic(vm) {}

protected:
	bool isInLoomSection() const override { return true; }
	void registerGameTools() override { registerPlayNoteTool(); }
	Common::JSONValue *dispatchGameTool(const Common::String &name, const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override {
		return dispatchPlayNote(name, args, errorOut, handled);
	}
	void applyGameVerbs(Common::JSONArray &verbsArr, Common::Array<VerbInfo> &activeVerbs,
	                    bool questionPending) override {
		applyLoomVerbs(verbsArr, activeVerbs, questionPending);
	}
};

class McpBridgeIndy3 : public McpBridgeClassic {
public:
	explicit McpBridgeIndy3(ScummEngine *vm) : McpBridgeClassic(vm) {}

protected:
	bool isInIndy3Fight() const override { return indy3FightActive(); }
	void augmentState(Common::JSONObject &out) override { addIndy3FightHud(out); }
};

class McpBridgeIndy4 : public McpBridgeClassic {
public:
	explicit McpBridgeIndy4(ScummEngine *vm) : McpBridgeClassic(vm) {}

protected:
	bool isInAtlantisBook() const override;
	void augmentStateObjects(Common::JSONArray &objects) override;
	bool interceptGameAct(const Common::JSONObject &args, Common::String &errorOut) override;

private:
	static const int kAtlantisBookPages = 5;
	// 1..kAtlantisBookPages for a "page_N" target name, 0 otherwise.
	static int atlantisBookPageFromName(const Common::String &name);
	// Turn the open book to page (1..kAtlantisBookPages) and stream its lines.
	bool turnAtlantisBookPage(int page, Common::String &errorOut);
};

class McpBridgeMonkey : public McpBridgeClassic {
public:
	explicit McpBridgeMonkey(ScummEngine *vm) : McpBridgeClassic(vm) {}

protected:
	// Monkey Island's "Open" (Öffne) is verb id 1, a real verb-bar entry the base
	// class would otherwise hide as the reserved default verb.
	bool includeBarVerbId1() const override { return true; }
	// Surface the Scumm Bar kitchen plank (an untouchable, unnamed hotspot) as
	// "plank"/"Planke" so it can be walked onto by name.
	Common::String syntheticObjectName(int numId) const override;

private:
	// True for the German (DE_DEU) build, so labels can be localised.
	bool isGerman() const;
};

class McpBridgePassport : public McpBridgeClassic {
public:
	explicit McpBridgePassport(ScummEngine *vm) : McpBridgeClassic(vm) {}

protected:
	bool isInLoomSection() const override { return loomSectionByVerbLabels(); }
	bool isInIndy3Fight() const override { return indy3FightActive(); }
	void registerGameTools() override { registerPlayNoteTool(); }
	Common::JSONValue *dispatchGameTool(const Common::String &name, const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override {
		return dispatchPlayNote(name, args, errorOut, handled);
	}
	void applyGameVerbs(Common::JSONArray &verbsArr, Common::Array<VerbInfo> &activeVerbs,
	                    bool questionPending) override {
		applyLoomVerbs(verbsArr, activeVerbs, questionPending);
	}
	void augmentState(Common::JSONObject &out) override { addIndy3FightHud(out); }
};

// --- V6: image-verb SCUMM (Sam & Max) --------------------------------------
class McpBridgeV6 : public ScummMcpBridge {
public:
	explicit McpBridgeV6(ScummEngine *vm) : ScummMcpBridge(vm) {}
};

class McpBridgeSamnMax : public McpBridgeV6 {
public:
	explicit McpBridgeSamnMax(ScummEngine *vm) : McpBridgeV6(vm) {}

protected:
	void applyGameVerbs(Common::JSONArray &verbsArr,
	                    Common::Array<VerbInfo> &activeVerbs, bool questionPending) override;
};

// --- V7: blast-text dialog SCUMM (The Dig, Full Throttle) ------------------
class McpBridgeV7 : public ScummMcpBridge {
public:
	explicit McpBridgeV7(ScummEngine *vm) : ScummMcpBridge(vm) {}

protected:
	// V7 dialog verb-script watcher: when the game switches to a dialog input
	// script (VAR_VERB_SCRIPT changes) the action is still in progress, so reset
	// the settle window to wait for the choices to appear.
	void pumpStreamGame() override;
	// V7 deferred clicks: the use-item scene click and the dialog-choice click.
	// Both Dig and Full Throttle inherit this.
	void pumpStreamGameLate() override;
};

class McpBridgeDig : public McpBridgeV7 {
public:
	explicit McpBridgeDig(ScummEngine *vm) : McpBridgeV7(vm) {}

protected:
	void applyGameVerbs(Common::JSONArray &verbsArr,
	                    Common::Array<VerbInfo> &activeVerbs, bool questionPending) override;
	bool resolveGameVerb(const Common::String &normalized, int &verbId) const override;
	// Dig pickup-deselect, then the shared V7 deferred clicks.
	void pumpStreamGameLate() override;
};

class McpBridgeFullThrottle : public McpBridgeV7 {
public:
	explicit McpBridgeFullThrottle(ScummEngine *vm) : McpBridgeV7(vm) {}

protected:
	void registerGameTools() override;
	Common::JSONValue *dispatchGameTool(const Common::String &name,
	                                    const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override;
	void applyGameVerbs(Common::JSONArray &verbsArr,
	                    Common::Array<VerbInfo> &activeVerbs, bool questionPending) override;
	bool resolveGameVerb(const Common::String &normalized, int &verbId) const override;
	bool dispatchGameAct(int verbId, int targetA, int targetB) override;
	bool pumpStreamGameEarly() override;

private:
	bool toolRideBike(const Common::JSONValue &args, Common::String &errorOut);

	// Bike fight (ride_bike): the fight runs inside INSANE's own loop and is
	// auto-played by Insane::_mcpAutoPilot. While this is set the stream stays
	// open (suppressing the usual room-change/settle close) until the fight
	// resolves (room 17 or 48) or a frame cap is hit.
	bool _sseFtRide = false;
	uint32 _sseFtRideGiveUpFrame = 0;
};

// --- V8: verb-slot SCUMM (Curse of Monkey Island) --------------------------
class McpBridgeV8 : public ScummMcpBridge {
public:
	explicit McpBridgeV8(ScummEngine *vm) : ScummMcpBridge(vm) {}

protected:
	// V8 settle-window extender: use-on-X actions chain follow-up scripts after
	// the sentence ends; bump the event frame whenever a new script appears so
	// the stream keeps settling while the chain runs.
	void pumpStreamGame() override;
};

class McpBridgeComi : public McpBridgeV8 {
public:
	explicit McpBridgeComi(ScummEngine *vm) : McpBridgeV8(vm) {}

protected:
	void registerGameTools() override;
	Common::JSONValue *dispatchGameTool(const Common::String &name,
	                                    const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override;
	void applyGameVerbs(Common::JSONArray &verbsArr,
	                    Common::Array<VerbInfo> &activeVerbs, bool questionPending) override;
	void augmentStateObjects(Common::JSONArray &objects) override;
	void augmentDebug(Common::JSONObject &out) override;
	void augmentStateChanges(Common::JSONObject &changes) const override;
	bool dispatchGameAct(int verbId, int targetA, int targetB) override;
	bool dispatchGameAnswer(const Common::Rect &slotRect, int verbid) override;
	bool resolveGameVerb(const Common::String &normalized, int &verbId) const override;
	void pumpStreamGame() override;
	void classifyGameEntity(int numId, bool &isPathway) const override;

private:
	bool toolShootCannon(const Common::JSONValue &args, Common::String &errorOut);
	// Cluster the room-4 war-canoe actor sprites into the boats still afloat,
	// ordered left-to-right; fills each cluster centre and a representative obj id.
	void collectCmiCannonBoats(Common::Array<Common::Point> &centers,
	                           Common::Array<int> &repObjs) const;

	// Cannon-minigame aim state (driven by toolShootCannon / pumpStreamGame).
	// _sseCannonAimX is the barrel's target column, _sseCannonAimY the elevation
	// index 0..12.
	bool _sseCannonAiming = false;
	int _sseCannonAimX = 0, _sseCannonAimY = 0;
	uint32 _sseCannonGiveUpFrame = 0; // safety cap on the barrel swing before firing
};

} // End of namespace Scumm

#endif
