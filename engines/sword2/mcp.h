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

#ifndef SWORD2_MCP_H
#define SWORD2_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Sword2 {

class Sword2Engine;

// MCP bridge for Broken Sword II: The Smoking Mirror.
//
// The two Broken Sword games play the same way — one click on a thing runs its
// interaction, a right click looks at it, and there is no verb bar — so this
// offers the same verbs as the first game's bridge and means the same thing by
// each of them. It is a separate class rather than a subclass because the two
// games are separate ScummVM engines: they share no types, and either can be
// built as a plugin without the other, so the only base they can have in common
// is MCP::McpBridge. What *is* shared is the wording of the tool surface, which
// the base class already owns.
//
// Where the two games differ is in what the engine will tell you. The first
// game attaches no name to anything on screen, so its bridge carries authored
// tables. This one names things itself: every mouse-detection box may carry a
// text line, which the game draws next to the cursor when the "object labels"
// option is on. The bridge reads that line straight out of the box whatever the
// option says, so the snapshot is labelled the way the game would label it — and
// what the option really controls is only whether a *player* sees it too.
//
// Every action is dispatched by replaying what Mouse::mouseEngine() does for a
// real click: the button flags, MOUSE_X/MOUSE_Y, CLICKED_ID and the player
// action event that the target's script picks up on the next cycle. Driving the
// game's own event rather than its scripts directly is what keeps walking,
// exits and cutaways behaving as they do for a player.
//
// All coordinates on the wire are world coordinates — the space the mouse
// boxes, PLAYER_FEET_X/Y and MOUSE_X/Y already agree on. It is scroll
// invariant, so a coordinate an agent remembers stays valid across a scroll.
class Sword2McpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static Sword2McpBridge *create(Sword2Engine *vm);

	explicit Sword2McpBridge(Sword2Engine *vm);
	~Sword2McpBridge() override;

	// Called from fnISpeak for every line the game says, whether or not
	// subtitles are on. `id` is the speaking object's resource id.
	void onSpeech(uint32 id, const char *text);

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
	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentChangesSchema(Common::JSONObject &props) override;

	// Reject every tool until the engine has built its subsystems and started
	// a game (the bridge is created in the constructor so the port binds
	// first), and every mutating one while the control panel owns the screen.
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
	bool isStreamStuck() const override;
	void pumpStreamTrack() override;

	// The engine runs its logic at about 12 cycles a second, and the bridge is
	// pumped once per cycle.
	uint32 minStreamFrames() const override { return 3; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 24 : 6; }
	uint32 timeoutFrames() const override { return 150; }
	uint32 absoluteTimeoutFrames() const override { return 900; }
	uint32 settleFrames() const override { return 4; }
	uint32 wallClockTimeoutMs() const override { return 180000; }
	// Anchor the deadline to the last sign of life: a cutaway can run long and
	// still be making progress.
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// A thing on screen the player could point at: one of the game's own
	// mouse-detection boxes.
	struct Hotspot {
		uint32 id;
		Common::String name;
		const char *kind;   // from the cursor the game would show
		bool isExit;
		bool isFloor;
		int priority;       // the game's own overlap-resolution order
		int x1, y1, x2, y2; // world coordinates
	};

	// A carried object, as the game's build_menu script reports it.
	struct Item {
		int32 icon;     // the id every script means by "this object"
		int32 luggage;  // what hangs off the cursor while it is held
		Common::String name;
	};

	// Frames the inventory cache may go without being rebuilt. Rebuilding
	// runs the game's own build_menu script, so it is done on a slow tick
	// rather than on every cycle.
	static const uint32 kInventoryRefreshFrames = 6;
	// Frames an action is given to show its first effect before it is taken
	// to have been a no-op.
	static const uint32 kNoOpFrames = 10;
	// Frames a skip is given to let its effect land. The click that cuts a
	// sequence short does not hand control back — the sequence may have
	// several more scenes to play — so a skip reports what one press did
	// rather than waiting for a game that is not going to answer.
	static const uint32 kSkipFrames = 8;
	// ... and the same window in wall-clock terms. A skip is most often sent
	// while a movie is playing, and a movie owns the loop: the game cycle that
	// the frame counter counts is not running at all, so the frame window
	// alone would never come round.
	static const uint32 kSkipMs = 700;

	// Are the engine subsystems built and a game running?
	bool engineReady() const;
	// Is the game accepting a new player action right now?
	bool canAct() const;
	// Is a line being said, or a subtitle up?
	bool speaking() const;
	// Where the player character stands, in world coordinates.
	void playerPos(int &x, int &y) const;
	// The current screen.
	uint32 location() const;
	Common::String locationName() const;
	// The resource the screen is drawn from, which is what actually carries
	// the screen's name and what changes when one is swapped for another.
	uint32 backgroundLayer() const;
	// True once a screen change has finished. The screen number changes a few
	// cycles before the new screen's background and mouse boxes exist, and a
	// snapshot taken in between still describes the screen just left.
	bool screenChangeSettled() const;

	// Everything pointable on this screen, names disambiguated.
	void collectHotspots(Common::Array<Hotspot> &out) const;
	// The detection box a click at a world point lands in, resolved the way
	// the game's own mouse code resolves it: lowest priority first, then the
	// order the screen listed them in. nullptr when the point is on nothing.
	// The strips that scroll the view are skipped: they sit on top of half
	// the screen, they move with the view, and they are a way of looking
	// around rather than something in the world.
	static const Hotspot *hotspotAt(const Common::Array<Hotspot> &hotspots, int x, int y);
	// The carried objects (from the cache pumpGame() keeps fresh).
	const Common::Array<Item> &inventory() const { return _inventory; }
	// Rebuild that cache from the game's own script.
	void refreshInventory();

	// The name the game itself would show for a thing: its label if it has
	// one, else the name authored on the resource, else object_<id>.
	Common::String nameForObject(uint32 id, int32 pointerText) const;
	// The authored name of a resource, folded into an identifier.
	Common::String resourceName(uint32 id) const;
	// The text line a label id points at.
	Common::String textLine(int32 textId) const;

	// Resolve a target name (or numeric id) to a thing on screen, or to a
	// carried item. Exactly one of the two is set on success.
	bool resolveTarget(const Common::String &name, Hotspot &hotspot, bool &isHotspot,
	                   Item &item, Common::String &errorOut) const;

	// Send a click to a thing on screen, warping the cursor there first so the
	// next cycle's hover bookkeeping agrees with what was clicked.
	void clickHotspot(const Hotspot &hotspot, int x, int y, bool rightButton);

	Sword2Engine *_vm;

	// The carried objects, refreshed on a slow tick by pumpGame().
	Common::Array<Item> _inventory;
	uint32 _inventoryFrame;

	// Speaking-object ids, indexed by the id pushMessage() carries.
	Common::Array<uint32> _messageActors;

	// What the stream in flight is: an action to see through, or a single
	// click whose effect is reported after a short fixed window.
	bool _skipStream;

	// Pre-action snapshot.
	int _ssePreLocation;
	uint32 _ssePreBackground;
	Common::Array<int32> _ssePreInventory;
	Common::Array<Common::String> _ssePreInventoryNames;
	Common::Array<Common::String> _ssePreHotspots;
	int32 _ssePreHeld;
	bool _sseActionStarted;
	// Last values pumpStreamTrack() compared against, so that only a real
	// change counts as progress.
	int _sseTrackPosX, _sseTrackPosY;
	int _sseTrackLocation;
	bool _sseTrackCanAct;
};

} // End of namespace Sword2

#endif
