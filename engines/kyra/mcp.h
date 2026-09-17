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

#ifndef KYRA_MCP_H
#define KYRA_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/hashmap.h"
#include "common/str.h"

namespace Kyra {

class KyraEngine_v1;
struct Button;

// MCP bridge for the Kyrandia engine (Kyrandia 1, 2 and 3).
//
// Kyrandia is a pointer game with one button and no verb bar. A click does
// whatever the thing under the pointer is for: an item lying in the room goes
// into the hand, a thing clicked with an empty hand is looked at or spoken to,
// and a thing clicked with an item in the hand has that item used on it. The
// hand is not the inventory: a carried item lives in one of the boxes along
// the bottom of the screen, and gets there by being clicked into one.
//
// So an action here is a short sequence of real clicks, replayed one at a time
// between passes of the game's own loop: empty the hand into a box, click the
// thing, put whatever ended up in the hand back into a box. 'take' is "click
// the item, then click a free box"; "use A on B" is "click A's box, click B,
// put A back". The bridge never moves an item itself.
//
// Names come from the game's own item-name table (Kyrandia 1 keeps a plain
// one, 2 and 3 keep theirs in the string files they print from). The four
// compass exits and the doorways the later games mark out have no names and
// are given ones; the characters standing in a Kyrandia 1 room are named by
// their slot, which is all the data says about them.
class KyraMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction. Returns
	// null for the engine's other games (Eye of the Beholder, Lands of Lore),
	// which share the engine class but not the scene model read here.
	static KyraMcpBridge *create(KyraEngine_v1 *vm);

	explicit KyraMcpBridge(KyraEngine_v1 *vm);
	~KyraMcpBridge() override;

	// Once per pass of the game's own loop - each of the three games has one
	// of its own, and they all call this. The clicks an action is made of are
	// only ever sent from here, so each lands on a game that has finished
	// dealing with the one before.
	void pump() override;
	// From the engine's delay(), which is where every one of its blocking
	// waits ends up: a cutscene, a spoken line, a fade - and the whole of a
	// walk. It advances the frame counter too, at most once per kFrameMs, so
	// the stream's budgets keep running while the game loop is not reached.
	void pumpFromStall();

	// Every line the game prints under the picture.
	void onGameText(const Common::String &text);
	// Every line somebody says: 0 is the hero, -1 somebody the game does not
	// say (a scene's own talking string).
	void onGameSpeech(int speaker, const Common::String &text);

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

	// Nothing here is typed, and no question is ever put as a numbered list:
	// conversations play themselves out.
	bool usesTypedInput() const override { return false; }
	bool usesDialogQuestions() const override { return false; }

	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentChangesSchema(Common::JSONObject &props) override;
	void augmentActSchema(Common::JSONObject &props) override;

	// --- Input injection ----------------------------------------------------
	void injectKey(const Common::KeyState &ks) override;
	void injectMouseMove(int x, int y) override;
	void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) override;

	// --- Text ---------------------------------------------------------------
	int currentRoomForMessages() const override;
	Common::String messageActorName(int actorId) const override;

	// --- Streaming ----------------------------------------------------------
	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;
	bool isActionDone() const override;
	bool hasPendingQuestion() const override;
	// Never true: an action that leads out of a room is not over when the
	// room changes. Arriving somewhere in Kyrandia very often starts a scene
	// (the first time out of Brandon's house is a minute-long speech), and a
	// stream that closed on the room change would leave all of it unread and
	// the caller refused with "not accepting input" for as long as it lasts.
	bool streamRoomChanged() const override { return false; }
	void pumpStreamTrack() override;
	void pumpStreamGame() override;
	void pumpGame() override;

	uint32 minStreamFrames() const override { return 6; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 90 : 20; }
	// Measured from the last sign of life (a line said, the hero moving, a
	// click sent), not from the start: a conversation can run for minutes and
	// is progressing the whole time.
	uint32 timeoutFrames() const override { return 3600; }
	uint32 absoluteTimeoutFrames() const override { return 0; }
	uint32 settleFrames() const override { return 20; }
	uint32 wallClockTimeoutMs() const override { return 120000; }
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}
	uint32 streamTimeoutAnchorMs() const override {
		return _sseLastEventMs > 0 ? _sseLastEventMs : _sseStartMs;
	}

private:
	enum TargetKind {
		kTargetItem,       // lying in the room
		kTargetCharacter,  // somebody standing in it
		kTargetExit,       // a way out
		kTargetCarried,    // in a box, or in the hand
		kTargetObject      // part of the picture the room's script answers a click on
	};

	// Something an agent can name.
	struct Target {
		Common::String name;
		Common::String label;   // what the game itself calls it, where it does
		TargetKind kind;
		int itemId;             // items only
		int x, y;               // where to click, in game coordinates
		bool held;              // carried: in the hand rather than in a box
		int slot;               // carried in a box: which one
	};

	// One step of an action. Each is checked against the game as it is when
	// its turn comes, so a step that is already true is simply dropped.
	enum StepKind {
		kStepStowHand,   // whatever is in the hand goes into a free box
		kStepHoldItem,   // the given item comes out of its box into the hand
		kStepClickAt,    // click a point of the room
		kStepClickBox,   // click the box holding the given item
		kStepPickUp      // click the given item lying at x, y, clear of whoever stands on it
	};
	struct Step {
		StepKind kind;
		int itemId;
		int slot;               // the box the item was in when the action began
		int x, y;
		uint attempts;
	};

	static const uint32 kFrameMs = 16;
	// Passes of the game loop the pointer rests on a spot before the click:
	// the later games decide what a click means from the cursor the hover
	// set, so the hover has to have been seen.
	static const uint kPointPasses = 3;
	// Passes of the game loop after a click before the next step is looked at.
	static const uint kAfterClickPasses = 3;
	// A step that has clicked this many times without becoming true is given
	// up on, and the rest of the action with it.
	static const uint kMaxStepAttempts = 4;
	// Frames with no pass of the game loop after which the game is taken to be
	// busy with something of its own (a walk, a scene, a speech).
	static const uint32 kLoopGoneFrames = 15;
	// Frames between two presses of the skip key while a scene plays on.
	static const uint32 kSkipRepeatFrames = 12;
	// How long after arriving a line under the picture is taken for the room's
	// name, as long as nothing has been clicked since.
	static const uint32 kRoomNameFrames = 240;

	void recoverAbandonedStream();

	bool engineReady() const;
	bool isFirstGame() const;   // Kyrandia 1, which keeps a different scene table
	bool isThirdGame() const;   // Kyrandia 3, whose inventory slides into view
	bool playerHasControl() const;

	int roomNumber() const;
	void heroPosition(int &x, int &y) const;
	int handItem() const;

	// The name the game prints for an item, or an empty string when it has
	// none.
	Common::String itemLabel(int itemId) const;
	Common::String itemName(int itemId) const;

	// Everything in the room an agent could click, named and disambiguated.
	void collectTargets(Common::Array<Target> &out) const;
	// What the player is carrying, the hand included (flagged 'held').
	void collectInventory(Common::Array<Target> &out) const;
	// Resolve a name against the room, then against what is carried.
	bool resolveTarget(const Common::String &name, Target &out) const;
	Common::String namesHere() const;
	// The later games: the things in the picture a click does something to,
	// found by asking the room's own click script (see the definition).
	void collectHotspots(const Common::Array<Target> &known, Common::Array<Target> &out) const;

	// --- The inventory boxes ---------------------------------------------
	int boxCount() const;              // boxes on screen at once
	int slotCount() const;             // slots the game keeps, on screen or not
	int slotItem(int slot) const;
	const Button *boxButton(int slot) const;
	bool boxCenter(int slot, int &x, int &y) const;
	int freeSlots() const;
	int findItemSlot(int itemId) const;
	// Kyrandia 3 only draws its boxes while the pointer is at the bottom.
	bool inventoryShown() const;

	// --- The step machine ----------------------------------------------------
	void queueStep(StepKind kind, int itemId = -1, int slot = -1, int x = 0, int y = 0);
	// Where a click picks up the item lying near x, y rather than landing on
	// the character standing over it; false when every such point is covered.
	bool itemClickClearOfCharacter(int itemId, int x, int y, int &clickX, int &clickY) const;
	// The box holding the item: the one it was in if it still is, else any.
	int slotFor(int itemId, int slot) const;
	void runSteps();
	// Put the pointer somewhere and queue the click that follows.
	void pointAndClick(int x, int y);
	// Move the pointer only, and count passes from now.
	void pointAt(int x, int y);
	bool clickBoxOrReveal(int slot, uint &attempts);
	void abandonSteps(const Common::String &why);

	KyraEngine_v1 *_vm;

	bool _inStallPump;
	uint32 _lastFrameMs;
	// Frame of the last pass of the game's own loop.
	uint32 _lastLoopFrame;
	// Passes of the loop since the last pointer move or click.
	uint _passesSinceInput;
	bool _skipStream;
	uint32 _lastSkipFrame;

	Common::Array<Step> _steps;
	bool _pendingClick;
	int _pendingX, _pendingY;
	// Why the last action stopped short, reported with its changes.
	Common::String _stepError;

	// The name each room printed on arrival, learned as rooms are visited.
	Common::HashMap<int, Common::String> _roomNames;
	int _nameRoom;
	uint32 _nameRoomFrame;
	bool _nameClicked;
	Common::String _startupName;

	// Pre-action snapshot, for the changes an action reports.
	int _ssePreRoom;
	int _ssePreX, _ssePreY;
	int _ssePreHand;
	Common::Array<Common::String> _ssePreTargets;
	Common::Array<Common::String> _ssePreInventory;
	// Progress tracking for the stream deadline.
	int _sseTrackRoom, _sseTrackX, _sseTrackY, _sseTrackHand;
	uint _sseTrackSteps;

	// The hotspots last found, for the room they were found in. Worked out
	// again when the room changes and after every action, which is what can
	// move, add or remove one; kept meanwhile, since an action in progress is
	// no time to be running the room's script.
	mutable Common::Array<Target> _hotspots;
	mutable int _hotspotRoom;
	mutable bool _hotspotsStale;
};

} // End of namespace Kyra

#endif
