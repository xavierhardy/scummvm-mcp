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

#ifndef TOON_MCP_H
#define TOON_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Toon {

class ToonEngine;

// MCP bridge for the Toon engine (Toonstruck).
//
// A two-button pointer game with no verb bar: a left click is whatever the
// thing itself does, a right click is a remark about it, and an object taken
// out of the bag can be carried in hand and used on something. The bridge
// exposes exactly that - use / look_at / walk_to, with an optional second
// target that is the item to do it with.
//
// Every action is a real click. The bridge parks the cursor on the target and
// presses a button, and the engine's own input handling does the rest: it
// resolves what was clicked exactly as it would for a player, walks the
// character over, and runs the thing's script. Acting any other way would
// take a different path than the one the game was written against.
//
// Names come from the game's own data. The line the game writes along the
// bottom of the screen when the player points at something names the scenery
// and the ways out; an item in the bag carries no name anywhere in the data,
// so the bridge remembers the name of whatever the item was taken from and
// calls it that from then on. Conversation options are icons with no text
// either, so each one is named after the line the player character will say
// when it is picked, read out of the conversation script without running it.
class ToonMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static ToonMcpBridge *create(ToonEngine *vm);

	explicit ToonMcpBridge(ToonEngine *vm);
	~ToonMcpBridge() override;

	// Called for every line the game says, whether or not subtitles show it.
	void onSpeech(int32 characterId, const char *text);
	// Called when the game puts something in the player's hand. The thing it
	// came from is the only place its name is ever written down, so this is
	// where an item gets named.
	void onItemInHand(int32 item);

	// Pump the transport from a secondary loop of the engine's, and get out of
	// the bag screen if the game opened it. That screen has a loop of its own
	// that nothing here can drive, and everything it offers is already in the
	// state snapshot, so it is closed on sight.
	void pumpTransport();

protected:
	// --- Tools --------------------------------------------------------------
	Common::JSONValue *toolState(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAct(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolWalk(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolSkip(const Common::JSONValue &args, Common::String &errorOut) override;
	Common::JSONValue *toolDebug(const Common::JSONValue &args, Common::String &errorOut) override;

	Common::String stateToolDescription() const override;
	Common::String actToolDescription() const override;
	Common::String answerToolDescription() const override;
	Common::String walkToolDescription() const override;
	Common::String skipToolDescription() const override;
	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentChangesSchema(Common::JSONObject &props) override;

	// Reject every tool until the engine has loaded a scene (the bridge is
	// built in the constructor so the port binds before the intro plays).
	Common::JSONValue *callTool(const Common::String &name,
	                            const Common::JSONValue &args,
	                            Common::String &errorOut) override;

	// --- Input injection ----------------------------------------------------
	void injectKey(const Common::KeyState &ks) override;
	void injectMouseMove(int x, int y) override;
	void moveCursorTo(int screenX, int screenY);
	void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) override;

	// --- Text ---------------------------------------------------------------
	Common::String messageActorName(int actorId) const override;
	int currentRoomForMessages() const override;

	// --- Per-frame ----------------------------------------------------------
	void pumpGame() override;

	// --- Streaming ----------------------------------------------------------
	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;
	bool isActionDone() const override;
	bool hasPendingQuestion() const override;
	bool streamRoomChanged() const override;
	bool pumpStreamGameEarly() override;
	void pumpStreamTrack() override;

	// The engine renders as fast as it can and calls the bridge once per
	// rendered frame, so a frame here is short - roughly 60 a second on this
	// machine, and never fewer than the 5/s the engine guarantees itself.
	// The budgets are scaled accordingly.
	uint32 minStreamFrames() const override { return 6; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 240 : 40; }
	uint32 timeoutFrames() const override { return 2400; }
	uint32 absoluteTimeoutFrames() const override { return 12000; }
	uint32 settleFrames() const override { return 45; }
	// A stream can outlive the frame counter entirely - a movie, a fade and a
	// scene load all run in loops of the engine's own, which pump the server
	// but produce no game cycle - so the last word is wall clock, set below
	// what a client will wait for, so a stuck action comes back as a failed
	// action rather than as a dropped connection.
	uint32 wallClockTimeoutMs() const override { return 45000; }
	// Anchor the deadline to the last sign of life: a scene can talk for a
	// long while and still be making progress.
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// Something on screen the player can point at.
	struct Target {
		Common::String name;
		int id;         // hotspot index, or one of the kThing* ids below
		bool isExit;    // leads to another location
		bool isCharacter;
		int x, y;       // where to put the cursor, in scene coordinates
		int x1, y1, x2, y2;
	};

	// The things that are not hotspots. The engine identifies them by these
	// same numbers in its own pointed-at bookkeeping.
	static const int kThingFlux = -3;
	static const int kThingDrew = -4;

	// What still has to happen before the queued click can be made.
	enum PendingPhase {
		kPhaseNone,
		kPhaseHand,    // put the right thing (or nothing) in the player's hand
		kPhaseScroll,  // bring the target into view
		kPhasePress,   // park the cursor and press
		kPhaseRelease  // let go again
	};

	// Frames the view is given to reach the target before the bridge presses
	// anyway. The view moves a few pixels a frame.
	static const uint32 kScrollFrames = 240;
	// Frames a skip is given to let its effect land: a skip reports what one
	// press did rather than waiting for whatever it cut short to unwind.
	static const uint32 kSkipFrames = 30;
	// Frames an action is given to show its first effect before it is taken
	// to have been a no-op.
	static const uint32 kNoOpFrames = 40;

	// Is the engine far enough along to be read?
	bool engineReady() const;
	// Is the game accepting a click right now?
	bool canAct() const;
	// Is the conversation window up and asking for a choice?
	bool inConversation() const;
	// Is the player character walking?
	bool leadMoving() const;
	// Where the player character stands, in scene coordinates.
	void leadPos(int &x, int &y) const;
	// How wide the current scene is, in scene coordinates.
	int sceneWidth() const;
	// The x the view would have to be at for a scene x to be on screen, or
	// the current one when it already is.
	int wantedScroll(int x) const;

	int sceneId() const;
	Common::String sceneName() const;

	// Everything pointable in the current scene, names disambiguated.
	void collectTargets(Common::Array<Target> &out) const;
	// The name the game writes for a hotspot, or an empty string.
	Common::String hotspotLabel(int index) const;
	// Is this hotspot one an agent can be offered? Aliases and switched-off
	// ones are not.
	bool hotspotUsable(int index) const;
	// A point inside the hotspot that a click would actually resolve to it,
	// the way the game resolves overlapping boxes. Returns false when
	// something else covers every part of it.
	bool hotspotClickPoint(int index, int &x, int &y) const;

	// The items in the bag, plus the one in hand.
	void collectInventory(Common::Array<int> &ids) const;
	// The name harvested for an item, or a fallback while unknown.
	Common::String rawItemName(int id) const;
	Common::String itemName(int id) const;
	// Remember an item under a name, if it has none yet.
	void nameItem(int32 item, const Common::String &name);

	// The conversation options, in the order their icons are drawn.
	void collectChoices(Common::Array<int> &slots) const;
	// The line option `slot` opens with, read out of the conversation script.
	Common::String choiceLabel(int slot) const;
	// The conversation options as {choices:[{id,label}]}.
	void buildQuestion(Common::JSONObject &question) const;

	// Resolve a target name (or numeric id) to something in the scene.
	bool resolveTarget(const Common::String &name, Target &out, Common::String &errorOut) const;
	// True when a click at (x, y) would be a plain "walk over there": nothing
	// claims the spot and the character can get to it.
	bool groundIsClear(int x, int y) const;
	// A spot on open ground to stop at beside `target`, as close to it as the
	// game leaves room for. False when it is walled in on every side.
	bool groundBeside(const Target &target, int &x, int &y) const;
	// The published name of whatever a click at (x, y) would act on, empty
	// when that is nothing.
	Common::String coveringName(int x, int y) const;
	// Resolve a target name (or numeric id) to a carried item.
	bool resolveItem(const Common::String &name, int &id) const;

	// Queue a click: get the hand right, bring the point into view, then
	// press there.
	void queueClick(int x, int y, bool rightButton, int32 wantHeld);
	// Take an item out of the bag / put the held one back, so the hand holds
	// what the queued click needs.
	void applyHand();
	// Hold the view still while a click is aimed, remembering what the game
	// had asked for so it can be put back afterwards.
	void pinView();

	ToonEngine *_vm;

	// Item id -> the name of the thing it was taken from.
	Common::Array<int32> _namedItemIds;
	Common::Array<Common::String> _namedItemNames;
	// The name of the thing the last click was aimed at, so that an item the
	// click produces can be named after it.
	Common::String _lastClickedName;

	// The click waiting on the hand, the view and the cursor.
	PendingPhase _pendingPhase;
	int _pendingX, _pendingY;
	bool _pendingRight;
	int32 _pendingHeld;     // item the hand must hold, or -1 for nothing
	int _pendingScrollTo;
	uint32 _pendingFrame;
	// Whether the bridge is holding the view still, and what the game's own
	// setting was before it did.
	bool _scrollPinned;
	// Half-way through the click that leaves the bag screen: the press is out,
	// the release is not.
	bool _escapePressed;
	bool _scrollWasLocked;

	// What the stream in flight is: a click to see through, or a single skip
	// whose effect is reported after a short fixed window.
	bool _skipStream;

	// Pre-action snapshot.
	int _ssePreScene;
	Common::Array<int> _ssePreInventory;
	Common::Array<Common::String> _ssePreInventoryNames;
	Common::Array<Common::String> _ssePreTargets;
	int _ssePreHeld;
	bool _sseActionStarted;
	// Last values pumpStreamTrack() compared against, so that only a real
	// change counts as progress.
	int _sseTrackScene;
	int _sseTrackPosX, _sseTrackPosY;
	bool _sseTrackControl;

	// Speaking character numbers, indexed by the id pushMessage() carries.
	Common::Array<int> _messageActors;
};

} // End of namespace Toon

#endif
