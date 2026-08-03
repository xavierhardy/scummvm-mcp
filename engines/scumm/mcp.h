/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SCUMM_MCP_H
#define SCUMM_MCP_H

#include "backends/networking/mcp/mcp_server.h"
#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/keyboard.h"
#include "common/rect.h"
#include "common/str.h"

namespace Scumm {

class ScummEngine;
class Actor;
struct ObjectData;

class ScummMcpBridge : public MCP::McpBridge {
public:
	// Factory: pick the bridge subclass for the running game. Games with their
	// own specialisation get a dedicated leaf class; every other game falls back
	// to the base class for its SCUMM engine version (McpBridgeV0 / McpBridgeClassic
	// / McpBridgeV6 / McpBridgeV7 / McpBridgeV8). The returned object is already
	// init()'d (tools registered) and ready to use.
	static ScummMcpBridge *create(ScummEngine *vm);

	explicit ScummMcpBridge(ScummEngine *vm);
	~ScummMcpBridge() override;

	// V7-only: invoked once per frame, just before the engine draws/clears the
	// blast text queue. The bridge snapshots dialog-choice text + click target
	// coordinates so toolState can expose the real labels and toolAnswer /
	// pumpStream can route the click to the correct screen position.
	virtual void onV7BlastTextSnapshot();

	// Accessor for protected getObjOrActorName used by helpers.
	const byte *callGetObjOrActorName(int obj) const;

	// set_talk_speed is SCUMM-only; everything else is dispatched by the base.
	Common::JSONValue *callTool(const Common::String &name,
	                            const Common::JSONValue &args,
	                            Common::String &errorOut) override;

protected:
	struct NamedEntity {
		enum Kind { kInventory, kObject, kActor };
		Kind kind;
		int numId;
		Common::String displayName;
		bool visible;
		bool isPathway = false;
	};

	struct ObjStateSnap {
		int objNr;
		int state;
	};

	// V7 dialog-choice snapshot (captured each frame). Full Throttle draws
	// choices as blast text; The Dig draws them as picture-icon blast objects,
	// in which case objNumber identifies the icon and x/y hold its on-screen
	// click target.
	struct V7Choice {
		Common::String text;
		int x;
		int y;
		int objNumber = 0;
	};

	// A verb exposed to the agent: numeric id plus its normalized name and the
	// human-readable label shown in state.verbs.
	struct VerbInfo { int verbId; Common::String name; Common::String label; };

	// --- Game/version hooks ------------------------------------------------
	// Called by the shared base implementations at the points where behaviour
	// used to branch on _vm->_game.id. Each leaf class (per game) overrides the
	// ones it needs; the base versions preserve the engine-generic behaviour.

	// Register any tools specific to this game (shoot_cannon, ride_bike, dial,
	// switch_character, play_note, …). Called at the end of registerTools().
	void registerGameTools() override {}
	// Add game-specific fields to the `state` tool's output schema.
	void augmentStateSchema(Common::JSONObject &outputProps) override { (void)outputProps; }
	// Handle a tool call the base callTool() did not recognise. Set handled=true
	// if consumed. Return value follows the IToolHandler contract (null for
	// streaming/void tools).
	Common::JSONValue *dispatchGameTool(const Common::String &name,
	                                    const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override {
		(void)name; (void)args; (void)errorOut; handled = false; return nullptr;
	}
	// Replace/augment the active verb list in toolState (e.g. a fixed fallback
	// verb set for single-cursor games). questionPending mirrors toolState's.
	virtual void applyGameVerbs(Common::JSONArray &verbsArr,
	                            Common::Array<VerbInfo> &activeVerbs, bool questionPending) {
		(void)verbsArr; (void)activeVerbs; (void)questionPending;
	}
	// Append synthetic scene objects to state.objects (e.g. CMI cannon boats).
	virtual void augmentStateObjects(Common::JSONArray &objects) { (void)objects; }
	// Append game-specific top-level fields to the state result (e.g. fight HUD).
	virtual void augmentState(Common::JSONObject &out) { (void)out; }
	// Append game-specific diagnostics to the debug tool result.
	virtual void augmentDebug(Common::JSONObject &out) { (void)out; }
	// Append game-specific fields to a streaming action's state-change result.
	virtual void augmentStateChanges(Common::JSONObject &changes) const { (void)changes; }
	// Dispatch a game-specific verb action inside toolAct's verb chain. Return
	// true if the action was handled (skips the default doSentence path).
	virtual bool dispatchGameAct(int verbId, int targetA, int targetB) {
		(void)verbId; (void)targetA; (void)targetB; return false;
	}
	// Dispatch a dialog-choice selection (toolAnswer) the game's own way (e.g.
	// CMI clicks the on-screen choice line). Return true if handled, else the
	// base runs the default verb-click input script. slotRect/verbid identify
	// the chosen verb slot.
	virtual bool dispatchGameAnswer(const Common::Rect &slotRect, int verbid) {
		(void)slotRect; (void)verbid; return false;
	}
	// Resolve a game-specific verb name to its engine verb id. Return true if
	// matched. `normalized` is the lower-cased, normalized verb name.
	virtual bool resolveGameVerb(const Common::String &normalized, int &verbId) const {
		(void)normalized; (void)verbId; return false;
	}
	// Per-frame game-specific streaming step (state machines: cannon aim, bike
	// fight, context-cursor clicks, …). Called from pumpStream() near the top,
	// right after the generic pre-step bookkeeping and before the close/timeout
	// checks. Use this for steps whose effects should be visible to the generic
	// settle/close logic on the same frame (e.g. resetting the settle window).
	void pumpStreamGame() override {}
	// Late per-frame game-specific streaming step, called from pumpStream() after
	// the generic timeout/button-clear logic and just before the settle/close
	// decision. Use this for deferred synthetic clicks (Dig pickup-deselect, the
	// V7 use-item and dialog-choice clicks) that must run after the button-clear
	// pass so they don't have their freshly-set button state cleared the same
	// frame. Default does nothing.
	void pumpStreamGameLate() override {}
	// Early per-frame streaming hook, run at the very top of pumpStream (before
	// the generic settle/close logic). Return true to end the frame immediately
	// (e.g. Full Throttle's bike fight, which runs inside INSANE's own loop and
	// only wants to wait for the section to resolve). Default does nothing.
	bool pumpStreamGameEarly() override { return false; }
	// Reset any game-specific per-stream state. Called from snapshotPreAction().
	virtual void resetGameStream() {}
	// True while a game-specific streaming state machine is still working (e.g.
	// Maniac Mansion still has queued dial-pad presses); keeps isActionDone()
	// from closing the stream early. Called from isActionDone().
	virtual bool gameStreamBusy() const { return false; }
	// Classify an entity for buildEntityMap (e.g. mark exit hotspots as pathway).
	// numId is the object/actor id; set isPathway to flag a navigable exit.
	virtual void classifyGameEntity(int numId, bool &isPathway) const { (void)numId; (void)isPathway; }
	// Verb id 1 is normally the engine's reserved default/sentence verb and is
	// hidden from the exposed verb bar. Games where id 1 is a real bar verb
	// (Monkey Island's "Open") override this to expose it. Default: hide it.
	virtual bool includeBarVerbId1() const { return false; }
	// True when the game drives its interface from the classic V3-V5 *text* verb
	// bar (labelled slots, dialog choices replacing the saved bar) rather than
	// the image/icon verb model Sam & Max introduced with V6. It is the SCUMM
	// version by default, but Day of the Tentacle is a V6 game that kept the
	// text bar, so its leaf overrides this to keep the V6 icon heuristics (verb
	// id -> canonical name, icon-dialog detection) from firing on it.
	virtual bool usesTextVerbBar() const;
	// Force an otherwise-unselectable/unnamed scene object into the entity map
	// under a stable, action-friendly name (e.g. Monkey Island's kitchen plank,
	// authored as an untouchable, unnamed hotspot). Return "" to leave the
	// object handled by the default selectability/name logic.
	virtual Common::String syntheticObjectName(int numId) const { (void)numId; return Common::String(); }
	// Optional human-readable name for an object's raw SCUMM state (e.g. a door
	// reported as "opened"/"closed"). Empty string means "no named state" and the
	// `state_name` field is omitted. `rawState` is _vm->getState(numId); isPathway
	// mirrors the object's computed pathway flag. The base implementation names
	// the state of any openable object (one that scripts both an open and a close
	// verb) as "opened"/"closed"; leaves may override to add game-specific states
	// (and can chain to ScummMcpBridge::objectStateName for the door default).
	virtual Common::String objectStateName(int numId, int rawState, bool isPathway) const;

	// Locate the engine verb ids for the "open" and "close" bar verbs (0 if the
	// game has none). Used for the generic door-state naming above.
	void findOpenCloseVerbIds(int &openVerb, int &closeVerb) const;

	// --- Protected accessors for ScummEngine internals ---------------------
	// The base class is the sole `friend` of ScummEngine; friendship is not
	// inherited, so subclasses reach the engine's protected members only through
	// these wrappers. (Public engine members — VAR(), _objs, _verbs, _virtscr,
	// _currentRoom, _screenWidth/Height, _system, … — are used directly.)
	// All const: they act through the _vm pointer, so the bridge's own const-ness
	// does not restrict calling (even non-const) engine methods on the pointee.
	int8 vmUserPut() const;
	Actor *vmActor(int i) const;
	// Null-safe + bounds-checked actor fetch (returns nullptr if the actor array
	// is unallocated or i is out of range).
	Actor *vmActorOrNull(int i) const;
	int vmNumActors() const;
	int vmNumVariables() const;
	int vmNumVerbs() const;
	// Null-safe SCUMM script variable read (0 if the var array is unallocated).
	int32 vmVar(int i) const;
	void vmConvertMessageToString(const byte *msg, byte *dst, int dstSize) const;
	int vmGetOwner(int obj) const;
	int vmGetState(int obj) const;
	int vmGetObjX(int obj) const;
	int vmGetObjY(int obj) const;
	int vmGetObjectIndex(int obj) const;
	int vmGetVerbEntrypoint(int obj, int entry) const;
	int vmActorToObj(int actor) const;
	int vmNumLocalObjects() const;
	// Null-safe inventory accessors (the array/count are engine-protected).
	uint16 *vmInventory() const;
	int vmNumInventory() const;
	// Number of script slots currently in use (V8 settle-window heuristic).
	int vmActiveScriptCount() const;
	void vmDoSentence(int verb, int objA, int objB) const;
	void vmRunInputScript(int clickArea, int val, int mode) const;
	void vmResetSentence() const;
	void vmActorFollowCamera(int actor) const;
	// V0 (Maniac C64) engine state — wraps ScummEngine_v0's protected members.
	// Only valid when _vm->_game.version == 0.
	bool v0InNormalMode() const;
	bool v0InKeypadMode() const;
	void v0SwitchActor(int slot) const;
	// Display name of an object/actor (empty if unnamed); wraps the protected
	// getObjOrActorName so leaf classes can use it.
	Common::String objName(int obj) const;
	Common::Point &vmMouse() const;
	Common::Point &vmVirtualMouse() const;
	uint32 &vmLastInputScriptTime() const;
	byte &vmLeftBtnPressed() const;
	byte &vmRightBtnPressed() const;

	// Sam & Max (V6): conversation topic icons are a row of floating objects in
	// the bottom verb strip, not blast objects. Refresh the dialog-choice list
	// (filtering the blank panel slots) from those objects.
	void collectSamnMaxDialogChoices(Common::Array<V7Choice> &out);

	// The engine, as its concrete type. MCP::McpBridge::_engine points at the
	// same object; keeping a typed alias here is what lets the SCUMM-specific
	// half of the bridge stay unchanged.
	ScummEngine *_vm;

	// Streaming (action/answer/walk) state beyond the shared base's.
	bool _sseEgoMoved;          // ego moved at any point during this stream
	// Loom's play_note hatches the egg into a long cutscene that keeps streaming
	// dialogue well past the usual pre-V7 timeout. Only that tool sets this, so
	// the relaxed per-event deadline never applies to other games' streams (e.g.
	// the Indy3 student-mob office, whose ambient argument would otherwise keep
	// resetting the deadline). Reset to false by snapshotPreAction().
	bool _sseAllowLongCutscene;
	int _sseTargetObject;       // V0: primary object acted on; 0 if none or non-V0
	Common::Array<uint16> _ssePreInventory;
	// Names captured at snapshot time so removed items can still be
	// reported by name after the engine unloads them (e.g. CMI inv-on-inv
	// combine consumes both inputs and the object data is no longer loaded).
	Common::Array<Common::String> _ssePreInventoryNames;
	// Last-seen inventory contents during a stream; bumps _sseLastEventFrame
	// whenever the set changes so the settling window extends through the
	// long deferred animations CMI dispatches after a sentence completes.
	Common::Array<uint16> _sseLastInventorySnapshot;
	uint                  _sseLastInventoryHashCount = 0;
	// Baseline of active script slots at action start. While the number of
	// active scripts exceeds this baseline (CMI chains animation/object
	// scripts after the sentence finishes), keep extending the settle window.
	int                   _ssePreActiveScriptCount = 0;
	int                   _sseLastActiveScriptCount = 0;
	Common::Array<ObjStateSnap> _ssePreObjectStates;
	bool _ssePendingSecondClick;
	int _sseClickMouseX, _sseClickMouseY;
	Common::Array<Common::KeyCode> _ssePendingNotes;
	// Last value seen in the Loom note variable (var 259). Used in pumpStream
	// to detect 0 -> note transitions and emit them as MCP notifications.
	int32 _ssePrevNoteValue;
	// Frame at which the most recent pending-note keypress was fed.
	uint32 _sseLastNoteFedFrame;

	// V7 talk-line polling state (see pump()).
	Common::String _lastV7TalkText;
	int _lastV7TalkActor = 0;

	// V7 dialog-choice cache, refreshed each frame from the blast-text queue.
	Common::Array<V7Choice> _v7DialogChoices;
	// Frame at which _v7DialogChoices was last refreshed with a non-empty
	// capture. Full Throttle draws conversation choices as blast text that the
	// dialog script re-enqueues every frame, whereas a one-shot cutscene line
	// (e.g. the bartender's last line in the keys sequence) is captured once and
	// then never refreshed. Used to expire such stale captures so they cannot
	// masquerade as a pending question and block movement after the cutscene.
	uint32 _v7DialogChoicesFrame = 0;

	// V7: VAR_VERB_SCRIPT "normal" value observed before first action; used to
	// detect when the game has switched to a dialog input handler.
	int _baseVerbScript = 0;
	// VAR_VERB_SCRIPT value at stream start (never updated during stream).
	int _sseInitialVerbScript = 0;
	// VAR_VERB_SCRIPT value at last observed change (updated in pumpStream).
	int _sseVerbScript = 0;
	// True once VAR_VERB_SCRIPT has changed from its initial stream value.
	bool _sseVerbScriptChanged = false;
	// V7: pending dialog choice digit (1-9); fed to the dialog script when
	// the game is ready to accept input. 0 means no choice is pending.
	int _ssePendingV7Choice = 0;
	// Sam & Max context-cursor click: object/actor id we are acting on by driving
	// the in-game verb cursor. While non-zero, pumpStream right-clicks to cycle the
	// context cursor to _sseSnmCursorTarget over the target, then left-clicks it.
	// Used for talk_to (cursor -> mouth) and for 'use' on objects with no verb-7
	// script such as the DeSoto (cursor -> use/operate). 0 means inactive.
	int _sseSnmTalkActor = 0;
	// The context-cursor icon id to cycle to before clicking (mouth 877 / use 878).
	int _sseSnmCursorTarget = 0;
	// Frame gate + right-click counter for the verb-cycle above.
	uint32 _sseSnmTalkNextFrame = 0;
	int _sseSnmTalkClicks = 0;
	// True once the virtual mouse has been pinned over the target for at
	// least one frame. The engine's hover detection must see the cursor over
	// the actor before a click counts as a click ON it, so the machinery
	// never left-clicks on the same pump frame that warped the mouse.
	bool _sseSnmHovered = false;
	// V7 use-item: deferred scene click after arming the inventory cursor.
	// The engine needs a frame between arming the cursor in the inventory
	// click handler and firing the scene click so the held-item state is
	// committed to script globals before the verb script reads it.
	bool _ssePendingV7UseClick = false;
	int  _ssePendingV7UseMouseX = 0;
	int  _ssePendingV7UseMouseY = 0;
	int  _ssePendingV7UseObjX = 0;
	int  _ssePendingV7UseObjY = 0;
	// Frame at which we should clear the simulated left/right-button msDown bit.
	uint32 _sseButtonClearFrame = 0;
	// The Dig: picking up a scene object leaves it held as the mouse cursor
	// (the game's held-item global, var 34, flips to 1), so every later click
	// becomes "use <item> on X". Since each MCP action is self-contained, we
	// deposit the item back into the inventory by simulating the player's
	// right-click once the pickup settles. Tracked per stream so it fires once.
	bool _sseDigDeselectDone = false;
	// Sam & Max two-target natural interface action: first click the scene
	// object/actor to put it in hand (e.g. Max -> cursor 889), then click this
	// target with the held cursor. 0 means no chained target is pending.
	int _sseSnmPendingUseTarget = 0;
	// When non-zero, keep VAR(177) pinned to this held cursor during the S&M
	// click state machine. Used for selecting Max from a closed inventory.
	int _sseSnmForcedCursor = 0;
	// True when streaming was triggered by toolAnswer() (dialog choice). For V8
	// the verb slots remain populated with dialog choices throughout, so the
	// hasPendingQuestion-based "done" signal cannot be used to close the stream
	// early; we must wait for the dialog response (actorTalk lines) to play out.
	bool _sseAnswerStream = false;

	// --- MCP::McpBridge overrides ------------------------------------------

	// Tool implementations
	Common::JSONValue *toolState(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAct(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolWalk(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolSkip(const Common::JSONValue &args, Common::String &errorOut) override;
	Common::JSONValue *toolDebug(const Common::JSONValue &args, Common::String &errorOut) override;
	// SCUMM defers to its own requestSave(), which the same scummLoop() iteration
	// processes later in the frame, rather than saving inline.
	Common::JSONValue *toolSaveState(const Common::JSONValue &args, Common::String &errorOut) override;
	Common::JSONValue *toolSetTalkSpeed(const Common::JSONValue &args, Common::String &errorOut);

	// Input injection
	void injectKey(const Common::KeyState &ks) override;
	void injectMouseMove(int x, int y) override;
	void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) override;

	// Text handling
	Common::String messageActorName(int actorId) const override;
	int currentRoomForMessages() const override;
	bool stripTalkieMetadata() const override;

	// Tool registration
	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentChangesSchema(Common::JSONObject &props) override;
	void registerDebugTools() override;

	// Per-frame engine step (V7 talk-line polling, S&M blast snapshot, …).
	void pumpGame() override;

	// Streaming
	bool streamRoomChanged() const override;
	bool isStreamStuck() const override;
	bool streamHadActivity() const override;
	uint32 stuckFrames(bool hadActivity) const override;
	uint32 absoluteTimeoutFrames() const override;
	uint32 streamTimeoutAnchor() const override;
	bool shouldCloseStream() const override;
	void pumpStreamTrack() override;
	void pumpStreamMid() override;
	void pumpStreamPreSettle() override;

	// Loom segment detection (full Loom or the Loom mini-game in Passport to
	// Adventure). Overridden by the Loom / Passport leaves.
	virtual bool isInLoomSection() const { return false; }

	// Indy3 fist-fight detection (full Indy3 or the Indy3 mini-game in Passport).
	// When true, toolState exposes a 'fight' object with both fighters' health
	// and punch-power gauges so an MCP client can react to the HUD. Overridden by
	// the Indy3 / Passport leaves.
	virtual bool isInIndy3Fight() const { return false; }

	// Fate of Atlantis "Lost Dialogue" close-up (Indy4): a full-screen, tabbed
	// reference book. When it is open toolState exposes its pages as synthetic
	// "page_N" objects plus the "book" itself, and toolAct turns to a page or
	// closes the book, so an MCP client can read the whole dialogue — including
	// the randomised Thera -> Atlantis heading — and leave the close-up again
	// using only the standard tools (no mouse/screenshot). Indy4 leaf.
	virtual bool isInAtlantisBook() const { return false; }

	// Intercept a whole act() call before verb resolution (e.g. Fate of Atlantis
	// turning a book page). Set 'handled' when the call belongs to the game hook,
	// and return whether it succeeded (errorOut describes a handled failure).
	// Default handles nothing.
	virtual bool interceptGameAct(const Common::JSONObject &args, Common::String &errorOut,
	                              bool &handled) {
		(void)args; (void)errorOut; handled = false; return false;
	}

	// Streaming helpers
	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;

	// Game-state helpers
	Actor *getEgoActor() const;
	bool hasPendingQuestion() const override;
	bool isActionDone() const override;

	void buildEntityMap(Common::Array<NamedEntity> &entities) const;
	bool resolveEntityByName(const Common::String &name, NamedEntity &out) const;
	bool resolveVerb(const Common::String &action, int &verbId) const;

	// Sam & Max: is this target the sidekick Max — addressed either as his actor
	// (id 3) or as the inventory tool "max_the_object"? Used to route a two-target
	// "use Max on Y" through the give sentence.
	bool snmIsMaxEntity(int obj) const;

	// Convert raw text in the game's native dialog code page (e.g. CP-850, CP-1252)
	// to UTF-8 so non-ASCII labels round-trip correctly through MCP JSON.
	// Falls back to byte-level sanitization when the engine is unavailable.
	Common::String safeUtf8(const Common::String &raw) const override;

	// Selectability helpers: mirror the engine's findObject() / getActorFromPos() rules
	// so that non-interactive entities are excluded from the MCP entity list.
	bool isObjectSelectable(const ObjectData &od) const;
	bool isActorSelectable(int actorId) const;
};

} // End of namespace Scumm

#endif
