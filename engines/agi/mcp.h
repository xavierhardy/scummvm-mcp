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

#ifndef AGI_MCP_H
#define AGI_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Agi {

class AgiEngine;

// MCP bridge for the AGI engine (King's Quest II and III, Police Quest).
//
// Every other bridge here drives a pointer game: the room holds things, the
// things have labels, and an action is a click on one of them. AGI is the
// generation before that and works nothing like it. Nothing on screen is
// labelled - a room is a picture with anonymous animated views walking about
// on it - and the player does not point at all. They type: "open the door",
// "look at the chest", "give the sandwich to the man". The interpreter parses
// that against a dictionary shipped with the game (WORDS.TOK) and matches it
// against the room's script.
//
// So the bridge is built around the two lists the game actually names, both
// of them words a person typed:
//
//   * the parser's dictionary, which is every word this game understands. An
//     agent that cannot see it is guessing at a vocabulary of a few hundred
//     words, and a wrong guess is indistinguishable from a wrong idea - the
//     game answers "I don't know how to do that" either way. state() carries
//     the verbs; the vocabulary tool carries the rest, because the full list
//     runs to several hundred words and does not belong in every snapshot.
//
//   * the OBJECT file, which names every item that exists and says where each
//     one is. The ones the player is carrying are the inventory; a name from
//     that list is a name the parser will certainly recognise.
//
// act() therefore composes a sentence and types it - verb "look_at" on target
// "chest" is "look at chest" - which is the same thing a player does and
// reaches the same parser. type_text is underneath it for anything the
// verb/target shape cannot say, and it is not a fallback: it is how this
// engine is played.
//
// Pumping: the interpreter's main loop calls processAGIEvents() once per
// cycle, which is the frame pump. It also stops dead inside AgiEngine::wait()
// - message boxes, "press a key", the inventory screen - and never reaches
// the loop again until the player answers, so that is a transport-only pump.
class AgiMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static AgiMcpBridge *create(AgiEngine *vm);

	explicit AgiMcpBridge(AgiEngine *vm);
	~AgiMcpBridge() override;

	// Once per interpreter cycle, from processAGIEvents().
	void pump() override;
	// From AgiEngine::wait(): the interpreter is blocked and will not reach
	// its loop until something answers. Services the server without advancing
	// the frame counter, and guards against walking back into itself.
	void pumpFromStall();

	// Every line the game prints, from TextMgr. The interpreter hand-wraps its
	// messages, so a line arrives as several rows of one sentence.
	void onGameText(const Common::String &text);

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
	Common::String typeTextToolDescription() const override;

	// The parser is the whole interface, so this is always on.
	bool usesTypedInput() const override { return true; }
	// AGI never puts a numbered choice to the player: a question is asked in
	// prose and answered by typing, which type_text already covers.
	bool usesDialogQuestions() const override { return false; }

	// The extra tool this engine needs: the dictionary the parser was built
	// with, which is too long to repeat in every state snapshot.
	void registerGameTools() override;
	Common::JSONValue *callTool(const Common::String &name,
	                            const Common::JSONValue &args,
	                            Common::String &errorOut) override;

	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentChangesSchema(Common::JSONObject &props) override;
	void augmentActSchema(Common::JSONObject &props) override;

	// --- Input injection ----------------------------------------------------
	void injectKey(const Common::KeyState &ks) override;
	// AGI has no pointer to inject. The mouse exists in ScummVM's AGI as a
	// convenience for walking and for the menu bar, and nothing an agent does
	// goes through it: every action here is typed. Both are implemented as
	// nothing rather than left out, because the base declares them for the
	// pointer engines and this one has to be concrete.
	void injectMouseMove(int x, int y) override;
	void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) override;

	// --- Text ---------------------------------------------------------------
	int currentRoomForMessages() const override;

	// --- Streaming ----------------------------------------------------------
	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;
	bool isActionDone() const override;
	bool hasPendingQuestion() const override;
	bool streamRoomChanged() const override;
	void pumpStreamTrack() override;

	// A frame is an interpreter cycle, and the interpreter runs at up to
	// 40 of them a second.
	uint32 minStreamFrames() const override { return 4; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 60 : 12; }
	uint32 timeoutFrames() const override { return 400; }
	uint32 absoluteTimeoutFrames() const override { return 1600; }
	uint32 settleFrames() const override { return 10; }
	// The interpreter stops running cycles while a message box is up, which is
	// exactly where a skip is sent. Real time is the only clock running there;
	// see McpBridge::wallClockCloseMs().
	uint32 wallClockTimeoutMs() const override { return 180000; }
	uint32 wallClockCloseMs() const override { return _skipStream ? kSkipMs : 0; }
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// One item from the OBJECT file, as an agent sees it.
	struct Item {
		Common::String name;   // published identifier
		Common::String label;  // what the OBJECT file actually says
		uint16 number;         // its index, which is what the game knows it by
		bool carried;
	};

	// How long a skip is given when the interpreter has stopped cycling.
	static const uint32 kSkipMs = 1500;
	// Frames a held direction key is kept down for one walk step. AGI moves
	// the ego while a direction is set and stops when it is cleared, so a walk
	// is "point him that way and let go after a while" rather than a
	// destination the engine knows about.
	static const uint32 kWalkFrames = 25;

	// Are the engine's subsystems built yet? The bridge is created before they
	// are, so the port binds before anything can block on startup.
	bool engineReady() const;
	// Is the interpreter taking commands - not in a cutscene, not blocked on
	// a message box?
	bool playerHasControl() const;

	int roomNumber() const;
	void egoPosition(int &x, int &y) const;
	// Everything in the OBJECT file, named and disambiguated.
	void collectItems(Common::Array<Item> &out) const;
	// The words this game's parser knows.
	void collectWords(Common::Array<Common::String> &out) const;

	// Type a sentence at the parser and press Return, the way the type_text
	// tool does. Used by act(), which is a sentence builder.
	void typeSentence(const Common::String &sentence);

	AgiEngine *_vm;

	// Set while pumpFromStall() is running, so a stall reached from inside the
	// pump does not pump again.
	bool _inStallPump;
	// Set for the duration of a skip's stream; see wallClockCloseMs().
	bool _skipStream;

	// A walk in progress: the direction being held and the frame it ends on.
	int _walkDirection;
	uint32 _walkUntilFrame;

	// Pre-action snapshot, for the changes an action reports.
	int _ssePreRoom;
	int _ssePreX, _ssePreY;
	int _ssePreScore;
	Common::Array<Common::String> _ssePreItems;
	// Progress tracking for the stream deadline.
	int _sseTrackRoom, _sseTrackX, _sseTrackY, _sseTrackScore;
};

} // End of namespace Agi

#endif
