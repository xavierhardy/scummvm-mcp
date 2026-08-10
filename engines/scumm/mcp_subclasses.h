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

protected:
	// The early SCUMM verb bar has no OBIM slots: verb id 1 is "Open", a real
	// bar verb in both Maniac Mansion and Zak McKracken. (V0 is already exempt
	// from the base class's id-1 skip; this extends the same treatment to the
	// V1/V2 ports, where "Open" would otherwise be missing from state.verbs.)
	bool includeBarVerbId1() const override { return true; }

	// Both early-SCUMM games have a phone with a keypad close-up, so `dial`
	// lives here rather than on the Maniac leaf.
	void registerGameTools() override;
	Common::JSONValue *dispatchGameTool(const Common::String &name,
	                                    const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override;
	void pumpStreamGame() override;
	void resetGameStream() override;
	bool gameStreamBusy() const override;

	// True while the dial pad this bridge can drive is on screen.
	bool dialPadOnScreen() const;
	bool toolDial(const Common::JSONValue &args, Common::String &errorOut);
	// Map each keypad key ("123456789*0#" order) to its button object for the
	// pad currently on screen. False when no pad is recognisable.
	bool collectDialPad(int objForKey[12]) const;
	// The verb the keypad buttons script (the one the game raises when the
	// player clicks a button); falls back to the bar's "push".
	int dialPadPressVerb(const int objForKey[12]) const;

	// Phone dial pad: queued button object ids to press one at a time, the verb
	// id used to press them, and the frame of the last press.
	Common::Array<int> _ssePendingDialObjs;
	int _sseDialVerbId = 0;
	uint32 _sseLastDialFedFrame = 0;
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
	void augmentChangesSchema(Common::JSONObject &props) override;
	void augmentStateChanges(Common::JSONObject &changes) const override;
	bool pumpStreamGameEarly() override;
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

	// --- Title screen: kid selection (choose_kids) -------------------------
	// The title screen is a row of kid portraits plus a START button, all plain
	// room objects with no names: the only thing that tells one portrait from
	// another is the "<Name> - <blurb>" line the game prints when it is clicked.
	// So choose_kids sweeps the row, reads each portrait's line, keeps the kids
	// the caller asked for and clicks the rest away again — no per-release table
	// of object ids or screen positions, and nothing that assumes a portrait
	// order. A clicked portrait carries its own selected flag
	// (kObjectStateIntrinsic), which is what tells a kept kid from a refused one.

	// A clickable title-screen object and the room-pixel point to click it at.
	struct KidPortrait {
		int obj;
		int x, y;
	};

	enum KidPhase {
		kKidIdle = 0,      // no selection in progress
		kKidClearSelected, // unselect whatever an earlier attempt left selected
		kKidAwaitClear,    // wait out one such click
		kKidClickPortrait, // click the next portrait in the sweep
		kKidAwaitPortrait, // wait for its "<Name> - ..." line, then keep or drop it
		kKidAwaitDeselect, // wait out the click that unselects an unwanted kid
		kKidClickStart,    // press START
		kKidAwaitStart,    // wait for the game to leave the title screen
		kKidSkipIntro      // escape through the intro until the player has control
	};

	// Kids that can join the leader (Dave), i.e. the team minus its leader.
	static const int kKidSideKicks = 2;

	// True (and fills the outputs) when the title screen's kid selection is up:
	// the largest row of identically sized objects is the portrait strip and the
	// odd object out is START.
	bool collectKidSelectScreen(Common::Array<KidPortrait> &portraits,
	                            KidPortrait &startButton) const;
	// Click a title-screen object the way a player would (V0-V2 recompute
	// VAR_VIRT_MOUSE_* from _virtualMouse, so this only sets the mouse itself).
	void clickKidScreenObject(const KidPortrait &target);
	// Name at the head of the first "<Name> - <blurb>" line captured since
	// *fromIndex*, normalized. False when no such line has arrived yet.
	bool kidNameFromMessages(uint fromIndex, Common::String &nameOut) const;
	// Abandon the sweep and fail the streaming call.
	void failKidSelection(const Common::String &reason);
	bool toolChooseKids(const Common::JSONValue &args, Common::String &errorOut);

	KidPhase _kidPhase = kKidIdle;
	Common::Array<KidPortrait> _kidPortraits;
	KidPortrait _kidStartButton = {0, 0, 0};
	Common::Array<Common::String> _kidWanted;  // asked for, not identified yet
	Common::Array<Common::String> _kidNamesSeen;   // every portrait name read
	Common::Array<Common::String> _kidChosen;      // the team, in click order
	uint _kidProbeIndex = 0;    // portrait the sweep is at
	uint _kidClearIndex = 0;    // portrait the pre-sweep clean-up is at
	int _kidSelected = 0;       // portraits we have switched on
	uint _kidMsgMark = 0;       // _sseMessages size when the click went out
	uint32 _kidPhaseFrame = 0;  // frame the current phase started
	int _kidSelectRoom = 0;     // the title screen's room
	bool _kidSkipIntro = true;
	int _kidEscapes = 0;             // escapes sent at the intro
	uint32 _kidLastEscapeFrame = 0;  // frame the last one went out
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
	static void addIndy3FightSchema(Common::JSONObject &outputProps);
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
	void augmentStateSchema(Common::JSONObject &outputProps) override {
		McpBridgeClassic::augmentStateSchema(outputProps);
		addIndy3FightSchema(outputProps);
	}
};

class McpBridgeIndy4 : public McpBridgeClassic {
public:
	explicit McpBridgeIndy4(ScummEngine *vm) : McpBridgeClassic(vm) {}

protected:
	bool isInAtlantisBook() const override;
	void augmentStateObjects(Common::JSONArray &objects) override;
	bool interceptGameAct(const Common::JSONObject &args, Common::String &errorOut,
	                      bool &handled) override;

private:
	static const int kAtlantisBookPages = 5;
	// 1..kAtlantisBookPages for a "page_N" target name, 0 otherwise.
	static int atlantisBookPageFromName(const Common::String &name);
	// True for the synthetic name that stands for the open book itself.
	static bool isAtlantisBookName(const Common::String &name);
	// The page the book is currently open at (0 if that cannot be determined).
	int atlantisBookCurrentPage() const;
	// The enabled tab ("paper clip") object of page N, 0 if none is enabled.
	int atlantisBookTabObject(int page) const;
	// Click *obj* the way a player would and stream what the click triggers.
	bool clickAtlantisBookObject(int obj, Common::String &errorOut);
	// Turn the open book to page (1..kAtlantisBookPages) and stream its lines.
	bool turnAtlantisBookPage(int page, Common::String &errorOut);
	// Shut the book, returning to the room it was opened from.
	bool closeAtlantisBook(Common::String &errorOut);
};

class McpBridgeMonkey : public McpBridgeClassic {
public:
	explicit McpBridgeMonkey(ScummEngine *vm) : McpBridgeClassic(vm) {}

protected:
	// Monkey Island's "Open" (Öffne) is verb id 1, a real verb-bar entry the base
	// class would otherwise hide as the reserved default verb.
	bool includeBarVerbId1() const override { return true; }
	// Surface the Scumm Bar kitchen plank (an untouchable, unnamed hotspot) as
	// "plank"/"Planke" so it can be walked onto by name, and the seagull's active
	// feeding/flight animation frame as "bird"/"Vogel".
	Common::String syntheticObjectName(int numId) const override;
	// Name the seagull's feeding/flight phase; falls back to the base door naming.
	Common::String objectStateName(int numId, int rawState, bool isPathway) const override;

private:
	// True for the German (DE_DEU) build, so labels can be localised.
	bool isGerman() const;
};

// Monkey Island 2 hides its verb bar on two kinds of screen: the island maps
// (click a destination to travel) and the swamp, once Guybrush is in the coffin
// (click where to row). Both leave the sentence line as the only label — "Row
// to" — and a click as the only interaction.
class McpBridgeMonkey2 : public McpBridgeClassic {
public:
	explicit McpBridgeMonkey2(ScummEngine *vm) : McpBridgeClassic(vm) {}

protected:
	bool usesClickOnlyScreens() const override { return true; }
	void applyGameVerbs(Common::JSONArray &verbsArr, Common::Array<VerbInfo> &activeVerbs,
	                    bool questionPending) override;
	bool interceptGameAct(const Common::JSONObject &args, Common::String &errorOut,
	                      bool &handled) override;

private:
	// The verb a click stands for on the current click-only screen, as the game
	// names it in the sentence line ("row to"), or "walk to" when it names none.
	Common::String clickVerbLabel() const;
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
	void augmentStateSchema(Common::JSONObject &outputProps) override {
		McpBridgeClassic::augmentStateSchema(outputProps);
		addIndy3FightSchema(outputProps);
	}
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

// Day of the Tentacle is V6 but kept the classic V3-V5 text verb bar (and the
// verb-id layout that goes with it), so it opts out of the V6 icon-verb and
// icon-dialog heuristics and names its verbs itself.
class McpBridgeTentacle : public McpBridgeV6 {
public:
	explicit McpBridgeTentacle(ScummEngine *vm) : McpBridgeV6(vm) {}

protected:
	bool usesTextVerbBar() const override { return true; }
	void applyGameVerbs(Common::JSONArray &verbsArr,
	                    Common::Array<VerbInfo> &activeVerbs, bool questionPending) override;
	bool resolveGameVerb(const Common::String &normalized, int &verbId) const override;

private:
	struct TentacleVerb { int id; const char *name; const char *label; };
	static const TentacleVerb kVerbs[];
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
	void augmentChangesSchema(Common::JSONObject &props) override;
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
