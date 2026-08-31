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


#ifndef SCI_MCP_H
#define SCI_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/rect.h"
#include "common/str.h"

#include "sci/engine/vm_types.h"

namespace Sci {

class SciEngine;

// MCP bridge for the SCI engine.
//
// Scope, deliberately narrow: the demos this covers are the ones that can be
// played at all. Sierra shipped most of its Quest-series demos as
// *non-interactive* ones — a slideshow that runs itself and takes no input —
// and there is nothing for an agent to do in those, so the bridge stays out of
// their way. The interactive ones are Gabriel Knight (SCI1.1) and Space Quest 6
// (SCI2.1), and both are icon-bar point-and-click games: no typed parser, one
// cursor that means a verb, and a click that means "do that here".
//
// What SCI gives a bridge that no other engine here does is a *name for
// everything*, straight out of the game data. Every object the interpreter
// knows carries the identifier its author typed — "gateDoor", "theShovel" —
// in its object header. So the snapshot never has to sweep a cursor over the
// screen to find out what things are called, the way the Broken Sword and
// Woodruff bridges do; it reads the cast list and asks each member its name.
// The cost of that is the opposite problem: the cast list also holds the
// game's own bookkeeping (movers, cyclers, timers, sound handles), and those
// are filtered out rather than offered as things to look at.
//
// Both games are pumped from the same two places the interpreter throttles
// itself: kGameIsRestarting for SCI16 and GfxFrameout::throttle() for SCI32.
class SciMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static SciMcpBridge *create(SciEngine *vm);

	explicit SciMcpBridge(SciEngine *vm);
	~SciMcpBridge() override;

	// Called for every line of game text as it is displayed, whether it is
	// spoken by an actor or printed as narration.
	void onGameText(const Common::String &text, int talkerId);

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
	void augmentChangesSchema(Common::JSONObject &props) override;

	// Neither game puts a list of things to say to the player: a conversation
	// here is watched, not chosen from. So `answer` is never registered, and
	// nothing an agent reads mentions it.
	bool usesDialogQuestions() const override { return false; }

	// Refuse every tool until the interpreter is running a room. The bridge is
	// built early so the port binds before the game's own start-up blocks.
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

	// The interpreter throttles itself to about 33 game cycles a second, and
	// the bridge is pumped once per cycle, so a frame here is a game cycle.
	uint32 minStreamFrames() const override { return 4; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 60 : 12; }
	uint32 timeoutFrames() const override { return 400; }
	uint32 absoluteTimeoutFrames() const override { return 1200; }
	uint32 settleFrames() const override { return 8; }
	uint32 wallClockTimeoutMs() const override { return 180000; }
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// Something in the room an agent can name: a cast member with a script
	// name that is not one of the interpreter's own bookkeeping objects.
	struct Target {
		Common::String name;
		reg_t object;
		int x, y;              // where to click, in game coordinates
		Common::Rect bounds;   // the object's "now seen" rectangle
	};

	// Frames the cursor is left on a target before the click is sent, so the
	// game's own hit-testing has seen the pointer arrive.
	static const uint32 kPointFrames = 2;
	// Frames a skip is given to let one Escape land.
	static const uint32 kSkipFrames = 12;

	// Is the interpreter far enough along to answer questions about a room?
	bool engineReady() const;
	// The room the game is in, as its own number and as its object's name.
	int roomNumber() const;
	Common::String roomName() const;
	// The score the game keeps, or -1 when it keeps none.
	int score() const;
	// Where the player character stands.
	bool egoPosition(int &x, int &y) const;
	// Is the player character walking?
	bool egoMoving() const;
	// Does the game have input turned on for the player right now?
	bool playerHasControl() const;

	// A global variable, as the interpreter holds it.
	reg_t global(int index) const;
	// The script-level name of an object, or an empty string.
	Common::String objectName(reg_t object) const;
	// One selector of an object, or `missing` when it has no such selector.
	int selector(reg_t object, int selectorId, int missing = 0) const;

	// Everything in the cast an agent could act on, names disambiguated.
	void collectTargets(Common::Array<Target> &out) const;
	// Resolve a name (or a numeric index into the snapshot) to a target.
	bool resolveTarget(const Common::String &name, Target &out, Common::String &errorOut) const;

	// Put the pointer somewhere and queue the click that follows once the
	// game has noticed it arrive.
	void pointAndClick(int x, int y, bool rightButton);
	// Send the queued click, if the pointer has been in place long enough.
	void pumpPendingClick();
	// Warp the pointer, and leave an event behind so a backend that draws
	// nowhere still reports the move.
	void moveCursorTo(int x, int y);

	SciEngine *_vm;

	// The click waiting on the pointer having been noticed.
	bool _pendingClick;
	bool _pendingRight;
	int _pendingX, _pendingY;
	uint32 _pendingFrame;

	// What the stream in flight is: an action to see through, or a single
	// Escape whose effect is reported after a short fixed window.
	bool _skipStream;

	// Pre-action snapshot.
	int _ssePreScore;
	Common::Array<Common::String> _ssePreTargets;
	// Last values pumpStreamTrack() compared against, so only a real change
	// counts as progress.
	int _sseTrackRoom;
	int _sseTrackPosX, _sseTrackPosY;
	int _sseTrackScore;

	// Talker numbers, indexed by the id pushMessage() carries.
	Common::Array<int> _messageActors;
};

} // End of namespace Sci

#endif
