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


#ifndef AGS_MCP_H
#define AGS_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace AGS {
class AGSEngine;
}

namespace AGS3 {

// MCP bridge for the AGS engine (Adventure Game Studio).
//
// AGS is the friendliest engine here to describe a room in, and for a reason
// none of the others share: it is a *toolkit*, so everything in a room was
// given a name by its author at design time and that name is still there at
// run time. A room object carries a display name meant for a player ("Front
// Door") and a script name meant for the game's code ("oFrontDoor"); so do
// hotspots, characters and inventory items. Nothing has to be swept for with
// a cursor the way the Broken Sword and Woodruff bridges must, and nothing
// has to be filtered out the way SCI's cast must - a room's object list is
// already only the things in the room.
//
// The verbs are the engine's own cursor modes, which is as close to a verb
// bar as AGS has: MODE_WALK, MODE_LOOK, MODE_HAND, MODE_TALK, MODE_USE,
// MODE_PICKUP. A game may leave some of them out, and the ones it kept are
// what `state` reports. An action sets the mode and clicks the target, which
// is exactly what a player does.
class AgsMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static AgsMcpBridge *create(::AGS::AGSEngine *vm);

	explicit AgsMcpBridge(::AGS::AGSEngine *vm);
	~AgsMcpBridge() override;

	// Called for every line the game displays, spoken or narrated.
	void onGameText(const Common::String &text, int charId);

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
	Common::String walkToolDescription() const override;
	Common::String skipToolDescription() const override;
	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	// select_verb, for the games whose verbs are buttons rather than cursors.
	void registerGameTools() override;
	Common::JSONValue *dispatchGameTool(const Common::String &name,
	                                    const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override;
	void augmentChangesSchema(Common::JSONObject &props) override;

	// AGS conversations are a list of things to say, and the engine draws them
	// itself in a modal loop. Reading that list back is a separate job from
	// this one, so `answer` is not registered and nothing refers to it.
	bool usesDialogQuestions() const override { return false; }

	// Refuse every tool until a room is loaded. The bridge is built early so
	// the port binds before the game's own start-up blocks.
	Common::JSONValue *callTool(const Common::String &name,
	                            const Common::JSONValue &args,
	                            Common::String &errorOut) override;

	// --- Input injection ----------------------------------------------------
	void injectKey(const Common::KeyState &ks) override;
	void injectMouseMove(int x, int y) override;
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
	void pumpStreamTrack() override;

	// AGS runs at its own configured frame rate, commonly 40 a second, and the
	// bridge is pumped once per game loop.
	uint32 minStreamFrames() const override { return 5; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 80 : 16; }
	uint32 timeoutFrames() const override { return 600; }
	uint32 absoluteTimeoutFrames() const override { return 1600; }
	uint32 settleFrames() const override { return 12; }
	uint32 wallClockTimeoutMs() const override { return 180000; }
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// Something in the room an agent can name.
	struct Target {
		Common::String name;
		Common::String kind;   // "object", "hotspot" or "character"
		int id;
		int x, y;              // where to click, in room coordinates
	};

	// Frames the cursor is left on a target before the click is sent, so the
	// game's own hit-testing has seen the pointer arrive.
	static const uint32 kPointFrames = 2;
	// Frames a skip is given to let one keypress land.
	static const uint32 kSkipFrames = 15;


	// Is a room loaded and the engine far enough along to be asked?
	bool engineReady() const;
	// The room the game is in.
	int roomNumber() const;
	// Where the player character stands.
	bool playerPosition(int &x, int &y) const;
	// Is the player character walking, or is the game otherwise busy?
	bool playerHasControl() const;

	// Everything in the room an agent could act on, names disambiguated.
	void collectTargets(Common::Array<Target> &out) const;
	// Somewhere inside hotspot *id* to click, found in the room's own hotspot
	// mask. Returns false when the hotspot covers nothing on screen.
	bool hotspotPoint(int id, int &x, int &y) const;
	// Resolve a name to something in the room.
	bool resolveTarget(const Common::String &name, Target &out,
	                   Common::String &errorOut) const;
	// The inventory the player is carrying, as names.
	void collectInventory(Common::Array<Common::String> &names,
	                      Common::Array<int> &ids) const;

	// One verb this game offers, and how to select it.
	struct Verb {
		Common::String name;
		int mode;        // the engine cursor mode, or -1 when there is none
		int guiId;       // the GUI holding the button, or -1
		int controlId;   // the button in it, or -1
		int x, y;        // where to click that button, in screen coordinates
	};

	// The verbs this game offers.
	//
	// Two shapes, and which one a game is decides how a verb is chosen. Most
	// AGS games use the engine's own cursor modes: the verb is the cursor, and
	// set_cursor_mode picks it. But a great many fan games - and both Zak
	// games here - build a SCUMM-style verb bar out of ordinary GUI buttons
	// labelled Look, Use, Pick up, Talk, Give, and drive their own verb state
	// from those. For those the cursor mode means nothing and the button has
	// to be clicked, exactly as a player clicks it.
	void collectVerbs(Common::Array<Verb> &verbs) const;
	// The verb bar's buttons, when this game has one. Empty when it does not.
	void collectVerbButtons(Common::Array<Verb> &verbs) const;
	// The verb the game says is selected, read off its own status line, or an
	// empty string when it does not keep one. A game with a verb bar writes
	// the current verb there - "walk", "look at", "pick up" - which is the
	// only place it is ever stated, and it is stated in the game's own words
	// rather than in a table kept here.
	Common::String currentVerbFromLabel() const;
	// The verb of that name this game offers, or false when it has none.
	bool findVerb(const Common::String &verb, Verb &out) const;
	// The verbs this game has, as a sentence to put in a refusal.
	Common::String verbList() const;

	// Point at a room position and queue the click that follows once the game
	// has seen the pointer arrive, having first selected *verb* - by setting
	// the cursor mode, or by clicking the verb bar's button.
	void pointAndClick(int x, int y, const Verb &verb);
	void pumpPendingClick();
	void moveCursorTo(int x, int y);

	::AGS::AGSEngine *_vm;

	// The click waiting on the pointer having been noticed, and the verb
	// button that has to be pressed before it.
	bool _pendingClick;
	int _pendingX, _pendingY;
	uint32 _pendingFrame;


	// What the stream in flight is.
	bool _skipStream;

	// Pre-action snapshot.
	Common::Array<Common::String> _ssePreTargets;
	Common::Array<Common::String> _ssePreInventory;
	int _sseTrackRoom;
	int _sseTrackPosX, _sseTrackPosY;

	// Speaking character numbers, indexed by the id pushMessage() carries.
	Common::Array<int> _messageActors;
};

} // End of namespace AGS3

#endif
