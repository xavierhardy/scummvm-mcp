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

	// One verb an icon-bar game offers, and the cursor that means it.
	// Public because the per-game tables are file-scope data in mcp.cpp,
	// where the reasoning that produced them belongs.
	//
	// A cursor is named two different ways depending on the game's age. The
	// later ones keep every cursor in one view and pick between them by loop;
	// the SCI1 ones name a cursor *resource* and have no loop at all, which is
	// what -1 means here. Both arrive through the same record - a game uses
	// one mechanism or the other, never both - so `view` is the resource
	// number for the older games and the view number for the newer ones.
	struct VerbCursor {
		const char *verb;
		int view;
		int loop;
	};

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

	// The early SCI games read a typed sentence rather than a click. Which
	// those are is the engine's to say - hasParser() - but it cannot be asked
	// yet: tools are registered while the interpreter is still starting up,
	// and hasParser() reads the SCI version, which is not set until a game is
	// loaded and asserts if asked before then. So the tool is registered for
	// every SCI game and refuses at call time on one that reads no typed
	// input, which is the only point at which the question can be answered.
	bool usesTypedInput() const override { return true; }

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
	// A skip is the action sent into an opening, which is where the older
	// games stop running cycles altogether, so it gets a short real-time
	// window. Every other action gets a long one for the same reason: a title
	// screen runs a handful of cycles a minute, so an action that looks done
	// there can never sit out its settle window in frames, and the stream
	// would run to the timeout above and come back as a failure rather than
	// as the nothing that actually happened.
	uint32 wallClockCloseMs() const override {
		return _skipStream ? kSkipMs : kStalledCloseMs;
	}
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
	// Frames between two presses of the button that cycles the verb, so each
	// one has been taken before the next is sent.
	static const uint32 kCycleFrames = 3;
	// Presses allowed before the bridge gives up on reaching a verb. The
	// cycle is short; anything past a couple of times round it is a game that
	// is not letting the verb change at all.
	static const int kCycleLimit = 24;
	// Frames a skip is given to let one Escape land.
	static const uint32 kSkipFrames = 12;
	// The same window in real time, for the loops where cycles do not run.
	// Twelve cycles is about a third of a second at the interpreter's own
	// rate; this is deliberately longer, because a game that is not cycling
	// is one whose reaction has not started yet.
	static const uint32 kSkipMs = 1500;
	// How long a settling action may wait on a frame counter that has all but
	// stopped before it is taken to be over. Far longer than a settle window
	// at any rate the interpreter really runs at, and short of the client's
	// own patience.
	static const uint32 kStalledCloseMs = 30000;

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

	// The list of everything on screen, followed through the Set object the
	// later SCI versions wrap it in. Null when there is none.
	reg_t castList() const;
	// Everything in the cast an agent could act on, names disambiguated.
	void collectTargets(Common::Array<Target> &out) const;
	// Resolve a name (or a numeric index into the snapshot) to a target.
	bool resolveTarget(const Common::String &name, Target &out, Common::String &errorOut) const;

	// The verbs this game offers, or nullptr when it is not one the bridge
	// has a table for. `count` is filled in either way.
	const VerbCursor *verbTable(uint &count) const;
	// The verb the cursor is showing now, or an empty string when the cursor
	// is not one of the verbs (an hourglass, a door-side arrow).
	Common::String currentVerb() const;
	// This game's entry for *verb*, or nullptr when it has no such verb.
	const VerbCursor *verbEntry(const Common::String &verb) const;
	// The verbs this game offers, as a sentence to put in a refusal.
	Common::String verbList() const;

	// Put the pointer somewhere and queue the click that follows once the
	// game has noticed it arrive. When *verb* is not nullptr the verb cursor
	// is cycled onto it first.
	void pointAndClick(int x, int y, bool rightButton, const VerbCursor *verb = nullptr);
	// Send the queued click, if the pointer has been in place long enough.
	void pumpPendingClick();
	// Warp the pointer, and leave an event behind so a backend that draws
	// nowhere still reports the move.
	void moveCursorTo(int x, int y);

	SciEngine *_vm;

	// The verb the queued click is waiting for the cursor to reach, or nullptr
	// when it is not waiting on one. The tables are file-scope constants, so
	// the pointer outlives every click made with it.
	const VerbCursor *_pendingVerb;
	int _cyclesSent;
	uint32 _cycleFrame;

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
