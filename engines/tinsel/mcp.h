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

#ifndef TINSEL_MCP_H
#define TINSEL_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

#include "tinsel/dw.h"
#include "tinsel/events.h"

namespace Tinsel {

class TinselEngine;

// MCP bridge for the Tinsel engine (Discworld and Discworld II).
//
// Both games are pointer games with no verb bar: the whole vocabulary is which
// button was pressed over what. A single left click walks there, a double left
// click is the action, a right click looks — and while an inventory item is
// held, the action is carried out *with* that item. The bridge exposes exactly
// that: walk_to / use / look_at, with an optional second target that is the
// item to do it with.
//
// What the player is pointing at is not a coordinate to the game, it is the
// thing the engine's own tag process last latched onto (a tagged actor, or a
// tag/exit polygon). So the bridge points before it acts: it puts the virtual
// cursor on the target, gives the tag process a few frames to notice, and only
// then raises the player event — the same event the keyboard bindings raise.
// Acting and pointing in the same breath would resolve the click against
// whatever was under the cursor beforehand.
//
// Names come from the game's own labels. Scenery and characters carry their
// tag string in the scene data, so the snapshot can read them straight out.
// An inventory item has no name in the data at all — the game only ever says
// what an item is by running the item's own script when the player points at
// it, which prints the name. The bridge harvests those names the same way: it
// runs that one script, intercepts the print (nothing is displayed) and keeps
// the name. It does so for items it has not named yet, a few frames at a time,
// while the game is idle.
//
// Conversations are icon windows rather than lines of text: the options are
// the contents of the conversation window, and choosing one is answer(id).
class TinselMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static TinselMcpBridge *create(TinselEngine *vm);

	explicit TinselMcpBridge(TinselEngine *vm);
	~TinselMcpBridge() override;

	// Called for every line an actor says, whether or not subtitles show it.
	void onSpeech(int actor, const char *text);
	// Called for every line of narration the game prints.
	void onPrint(const char *text);
	// Called from the inventory-object print path. While the bridge is
	// harvesting names it takes the string and returns true, which stops the
	// game displaying it — the harvest runs the script only to read the name.
	bool takeObjectName(SCNHANDLE hText, int objectId);

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
	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentChangesSchema(Common::JSONObject &props) override;

	// Reject every tool until the engine has built its subsystems and loaded a
	// scene (the bridge is created in the constructor so the port binds first).
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
	bool pumpStreamGameEarly() override;
	void pumpStreamTrack() override;

	// The engine runs one game cycle per GAME_FRAME_DELAY (ONE_SECOND ticks a
	// second), and the bridge is pumped once per cycle, so a frame here is a
	// game tick. The budgets match the other pointer-game bridges' timings.
	uint32 minStreamFrames() const override { return 4; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 36 : 8; }
	uint32 timeoutFrames() const override { return 300; }
	uint32 absoluteTimeoutFrames() const override { return 900; }
	uint32 settleFrames() const override { return 6; }
	uint32 wallClockTimeoutMs() const override { return 180000; }
	// Anchor the deadline to the last sign of life: a scene can talk for a
	// long while and still be making progress.
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// Something on screen the player can point at: a tagged actor, or a
	// tag/exit polygon.
	struct Target {
		Common::String name;
		int id;        // actor number, or polygon handle
		bool isActor;
		bool isExit;
		int x, y;      // where to put the cursor, in scene coordinates
	};

	// Frames to leave the cursor on a target before raising the player event,
	// so the tag process has run and latched onto it.
	static const uint32 kPointFrames = 3;
	// Frames a batch of name scripts is given to answer before the objects
	// left in it are written off as nameless (so they are not retried every
	// frame). The scripts answer on the next scheduler pass or not at all.
	static const uint32 kHarvestGraceFrames = 4;
	// Name scripts asked at once. They only answer a question, so a handful
	// in flight gets the sweep over with in a couple of seconds.
	static const uint32 kHarvestBatch = 4;
	// Frames an action is given to show its first effect before it is taken
	// to have been a no-op.
	static const uint32 kNoOpFrames = 16;
	// Frames a scroll is given to reach its destination before the bridge
	// stops waiting for it. A scroll moves a handful of pixels per frame, so
	// this is generous enough for the widest scene either game has.
	static const uint32 kScrollFrames = 240;
	// How close to the edge of the screen a point may sit and still be
	// pointed at. A tag area is bigger than the spot the bridge picks in it,
	// so leave room for the cursor to land inside it.
	static const int kScrollMargin = 24;
	// Frames a skip is given to let its effect land. Escape hands control
	// back only at the end of the sequence it cut short, which may be several
	// scenes away, so a skip reports what one press did rather than waiting.
	static const uint32 kSkipFrames = 12;

	// Are the engine's subsystems built and a scene loaded?
	bool engineReady() const;
	// Is the game accepting a new player action right now?
	bool canAct() const;
	// Is the conversation window up and asking for a choice?
	bool inConversation() const;
	// Is the lead character walking?
	bool leadMoving() const;
	// Where the lead character stands, in scene coordinates.
	void leadPos(int &x, int &y) const;

	// The current scene, as the engine's own handle table names it.
	int sceneId() const;
	Common::String sceneName() const;

	// Everything pointable in the current scene, names disambiguated.
	void collectTargets(Common::Array<Target> &out) const;
	// The items in the two inventories, plus the held one.
	void collectInventory(Common::Array<int> &ids, Common::Array<Common::String> &names) const;
	// The conversation window's options, in display order.
	void collectChoices(Common::Array<int> &ids) const;

	// The name harvested for an inventory item, or a fallback while unknown.
	Common::String itemName(int id) const;
	// The conversation options as {choices:[{id,label}]}, the last one being
	// the way out of the conversation.
	void buildQuestion(Common::JSONObject &question) const;
	// Ask the next unnamed inventory object for its name, if the game is idle.
	void pumpNameHarvest();

	// Resolve a target name (or numeric id) to something in the scene.
	bool resolveTarget(const Common::String &name, Target &out, Common::String &errorOut) const;
	// Resolve a target name (or numeric id) to an inventory item id.
	bool resolveItem(const Common::String &name, int &id) const;

	// Bring a scene position into view if the scene is wider or taller than
	// the screen and the position lies outside it. Returns true when a scroll
	// was asked for, i.e. the caller has to wait before pointing.
	bool requestScroll(int x, int y) const;
	// Point the cursor at a scene position and queue the player event that
	// follows once the tag process has caught up. Scrolls the view onto the
	// position first if it is off screen.
	void pointAndQueue(int x, int y, PLR_EVENT event);

	// Register a speaking actor's name for the message queue.
	int actorSlot(int actor);

	TinselEngine *_vm;

	// Item id -> harvested name, as two parallel arrays. An entry with an
	// empty name means the script ran and named nothing, so it is not retried.
	Common::Array<int> _namedItemIds;
	Common::Array<Common::String> _namedItemNames;
	// The objects whose name script is in flight, so takeObjectName() consumes
	// their print instead of letting the game show it.
	Common::Array<int> _harvestPending;
	uint32 _harvestFrame;
	// How far the sweep over the game's inventory objects has got.
	int _harvestNext;

	// What the stream in flight is: an action to see through, or a single
	// Escape whose effect is reported after a short fixed window.
	bool _skipStream;

	// The player event waiting on the cursor having been noticed.
	bool _pendingEvent;
	PLR_EVENT _pendingKind;
	uint32 _pendingFrame;
	// Where that event is aimed, and whether the view still has to scroll
	// onto it before the cursor can be put there.
	int _pendingX, _pendingY;
	bool _pendingScroll;

	// Pre-action snapshot.
	int _ssePreScene;
	Common::Array<int> _ssePreInventory;
	Common::Array<Common::String> _ssePreInventoryNames;
	Common::Array<Common::String> _ssePreTargets;
	int _ssePreHeld;
	bool _sseActionStarted;
	// Last values pumpStreamTrack() compared against, so that only a real
	// change counts as progress. A condition that simply stays true (control
	// off through a whole cutscene) must not keep the deadline alive.
	int _sseTrackScene;
	int _sseTrackPosX, _sseTrackPosY;
	bool _sseTrackControl;

	// Speaking actor numbers, indexed by the id pushMessage() carries.
	Common::Array<int> _messageActors;
};

} // End of namespace Tinsel

#endif
