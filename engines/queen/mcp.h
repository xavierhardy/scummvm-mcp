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

#ifndef QUEEN_MCP_H
#define QUEEN_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

#include "queen/defs.h"

namespace Queen {

class QueenEngine;

// MCP bridge for Flight of the Amazon Queen.
//
// FOTAQ is a verb-panel game (open/close/move/give/use/pick up/talk to/look
// at), so `act` maps almost one-to-one onto the game's own command machinery:
// the bridge resolves targets to the same subject encoding the panel produces
// (positive = room object number, negative = inventory item number), hands the
// finished command to Command::mcpExecute(), and the engine's main loop
// executes it exactly as if the player had built it with the mouse — including
// walking Joe over, cutaways and dialogues.
//
// Dialogues run inside Talk::selectSentence()'s modal loop; that loop already
// polls Input::keyVerb() for the digit shortcuts, so `answer` simply injects
// the matching digit verb. The pending options are published to the bridge by
// a hook in selectSentence() and surface as state.question.
class QueenMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static QueenMcpBridge *create(QueenEngine *vm);

	explicit QueenMcpBridge(QueenEngine *vm);
	~QueenMcpBridge() override;

	// Called from Talk::speak() for every line said in-game (Joe, actors,
	// cutaway dialogue). `sentence` is raw and may carry *XY command codes.
	void onSpeech(const char *actorName, const char *sentence);

	// Called from Talk::selectSentence() when a set of dialogue options is
	// (re)displayed, and with an empty count when the selection is over.
	void onTalkOptions(const char options[5][256], int count);
	void onTalkOptionsDone();

protected:
	// --- Tools --------------------------------------------------------------
	Common::JSONValue *toolState(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAct(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolWalk(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolSkip(const Common::JSONValue &args, Common::String &errorOut) override;
	Common::JSONValue *toolDebug(const Common::JSONValue &args, Common::String &errorOut) override;

	Common::String stateToolDescription() const override;
	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;

	// Reject every tool until the engine's subsystems exist (the bridge is
	// created in the QueenEngine constructor, before run() builds them).
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

	// --- Streaming ----------------------------------------------------------
	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;
	bool isActionDone() const override;
	bool hasPendingQuestion() const override;
	bool streamRoomChanged() const override;
	void pumpStreamTrack() override;

	// The engine updates at ~10 Hz (Input::DELAY_NORMAL 100 ms), about the
	// same rate as the other non-SCUMM bridges, so the budgets match theirs.
	uint32 minStreamFrames() const override { return 3; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 18 : 4; }
	uint32 timeoutFrames() const override { return 100; }         // ~10 s without an event
	uint32 absoluteTimeoutFrames() const override { return 900; } // ~90 s
	uint32 settleFrames() const override { return 3; }
	uint32 wallClockTimeoutMs() const override { return 180000; }
	// Anchor the deadline to the last sign of life: cutaways and dialogues run
	// long while still progressing.
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// A named thing the player can target.
	struct RoomObject {
		int16 relNum;   // room-relative object number (grid zone)
		uint16 absNum;  // index into Logic::_objectData
		Common::String name;
		bool isPerson;
		bool isExit;
		uint16 x, y;
	};

	// Is the game accepting a new player command right now?
	bool canAct() const;

	// Enumerate the current room's visible, named objects (including persons),
	// names normalized and de-duplicated (crate, crate_2, …).
	void collectRoomObjects(Common::Array<RoomObject> &out) const;

	// Current inventory as item numbers, with normalized names.
	void collectInventory(Common::Array<uint16> &items,
	                      Common::Array<Common::String> &names) const;

	// Clamp a walk target to the walkable extent of the current room, as the
	// `walk` tool's description promises.
	void clampToRoom(int &x, int &y) const;

	// Resolve a tool target to the panel's subject encoding: > 0 room object
	// (absolute), < 0 inventory item (-itemNum). `relNum` carries the room-
	// relative number for room objects (0 for items).
	bool resolveTarget(const Common::String &name, int16 &subject, int16 &relNum,
	                   Common::String &errorOut) const;

	// Register an actor name and return its index (messages carry ints).
	int actorId(const Common::String &name);

	QueenEngine *_vm;

	// Speaking-actor names, indexed by the id pushMessage() carries.
	Common::Array<Common::String> _actorNames;

	// Dialogue options currently on offer (empty = no pending question).
	Common::Array<Common::String> _talkOptions;

	// Pre-action snapshot.
	int _ssePreRoomNum;
	Common::Array<uint16> _ssePreInventory;
	Common::Array<Common::String> _ssePreInventoryNames;
	Common::Array<RoomObject> _ssePreObjects;
	bool _sseActionStarted;
};

} // End of namespace Queen

#endif
