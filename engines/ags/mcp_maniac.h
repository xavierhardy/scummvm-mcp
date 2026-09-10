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

#ifndef AGS_MCP_MANIAC_H
#define AGS_MCP_MANIAC_H

#include "ags/mcp.h"

namespace AGS3 {

// MCP bridge for Maniac Mansion Deluxe, the AGS remake of Maniac Mansion.
//
// It plays like any other AGS game once it has started, and everything in this
// class is about the two things it does that no other AGS game here does.
//
// The first is its opening screen. Before a room is ever entered the game
// draws a row of seven portraits, a START button and a line asking for two
// more kids; Dave is picked already and cannot be unpicked. None of that is
// describable through `state`: the portraits are unnamed hotspots over a
// painted title, so a snapshot of that screen is an empty room. `choose_kids`
// is the tool for it, and mirrors the one the original game's bridge offers
// (engines/scumm/mcp_v0.cpp) so an agent that has played one has learnt the
// other: name the two kids joining Dave, and the call returns with the game
// begun.
//
// It is driven differently, though. Both games name a portrait only when it is
// clicked - "<Name> - <blurb>", printed by the original and written on a GUI
// label by this one - so the original's bridge learns the row by clicking every
// portrait in it and clicking the unwanted ones off again. This one does not
// have to: the cast is in the game's own character table, in the order the
// portraits are drawn, so which slot is which kid is known before anything is
// pressed. The two wanted portraits are clicked, and the line each click writes
// is read back to check that it really was that kid. If one ever disagrees, the
// answer is believed over the table and the row is swept the long way from
// there, so a release that shuffled its portraits costs clicks rather than
// picking the wrong team.
//
// The screen also has a clock on it. Left alone for a minute the game starts
// showing itself off — the demo it plays in an arcade, the same rooms with
// nobody in charge — which is a minute an agent may well spend thinking. So
// nothing here is written as though the screen will still be there: one escape
// brings it back, `choose_kids` sends that escape itself, and `state` goes on
// saying `kid_selection_pending` for as long as the team is short of three,
// which is true whether the screen is up or the game is playing itself.
//
// The second is the team itself. Three kids are in play and the interface
// switches between them with two portrait buttons at the right-hand end of the
// verb bar - the two who are *not* being controlled, so which button is which
// kid changes every time one is pressed. `switch_character` presses them and
// reads back who it got, and `state` reports the team in `available_characters`
// and who has control in `controlling`.
class AgsMcpBridgeManiacDeluxe : public AgsMcpBridge {
public:
	explicit AgsMcpBridgeManiacDeluxe(::AGS::AGSEngine *vm);

protected:
	void registerGameTools() override;
	Common::JSONValue *dispatchGameTool(const Common::String &name,
	                                    const Common::JSONValue &args,
	                                    Common::String &errorOut, bool &handled) override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentState(Common::JSONObject &out) override;
	void augmentChangesSchema(Common::JSONObject &props) override;
	void augmentStateChanges(Common::JSONObject &changes) const override;
	void snapshotPreAction() override;
	// The verb bar. Nine buttons in three rows, drawn with the verb words
	// painted into the sprites, so the game labels nothing and the base class
	// has no name for any of them.
	void nameVerbButtons(Common::Array<Verb> &buttons) const override;
	void addGameVerbs(Common::Array<Verb> &verbs) const override;
	Common::String selectedVerb() const override;
	bool pumpStreamGameEarly() override;
	// The game hides its whole interface whenever it is telling the story -
	// the opening screen, the intro, and every cutscene in between - and puts
	// it back the moment the player is in charge again. That is a far better
	// answer to "can I act?" than the walk flag alone, which reads true all
	// the way through an intro nobody can interrupt.
	bool playerHasControl() const override;

private:
	// --- The title screen --------------------------------------------------

	// One portrait: the hotspot that answers for it, a point inside that
	// hotspot to put the cursor on, and the room object drawn as the frame
	// around it - whose `on` flag is the game's own record of whether this kid
	// has been picked.
	struct KidSlot {
		int hotspot;
		int x, y;
		int frameObj;
	};

	// The kids that join the leader. Dave leads the rescue and is picked from
	// the start, so the screen is waiting for two more.
	static const uint kKidSideKicks = 2;
	// The whole cast: the seven kids are the game's first seven characters, in
	// the order their portraits are drawn in. Everyone after them is someone
	// the game moves itself.
	static const int kKidCharacters = 7;
	// A row of fewer than this many portraits is not the selection screen.
	static const uint kMinPortraits = 4;

	enum Phase {
		kIdle = 0,       // nothing in progress
		kReturnToTitle,  // escape out of the game's own demo, back to the screen
		kPickChoose,     // decide which portrait to click next
		kPickClick,      // click it
		kPickRead,       // read the line it writes, and whether it took
		kPickUndo,       // click a kid we did not want back off again
		kStartClick,     // press START
		kStartAwait,     // wait for the game to leave the title screen
		kSkipIntro,      // escape through the opening until control is handed over
		kSwitchClick,    // press a portrait button on the verb bar
		kSwitchAwait     // wait to see which kid it gave us
	};

	// True (and fills the outputs) while the selection screen is up: the row of
	// evenly spaced hotspots is the portrait strip, the odd hotspot out is
	// START, and the largest set of room objects sharing one sprite is the row
	// of frames drawn around picked kids.
	bool collectKidSelectScreen(Common::Array<KidSlot> &slots, int &startX, int &startY) const;
	// Whether this slot is picked, read off its frame object.
	bool slotPicked(const KidSlot &slot) const;
	// Whether the team has been settled: three kids in play. False on the
	// opening screen and all the way through the demo the game plays itself,
	// where Dave is the only kid the game has put anywhere.
	bool teamPicked() const;
	// The name at the head of the "<Name> - <blurb>" line the game writes on a
	// GUI label for the portrait last clicked, normalized. Empty when no such
	// line is showing.
	Common::String shownKidName() const;
	// The seven kids, as the game names them, in the order their portraits are
	// drawn. Empty when the game has fewer characters than that.
	void collectCast(Common::Array<Common::String> &names) const;
	// The kids in play, as the game names them, and which one has control.
	void collectTeam(Common::Array<Common::String> &names, Common::Array<int> &ids) const;
	Common::String controllingKid() const;
	// A point to put the cursor on. The opening screen does not scroll, so
	// room and screen coordinates are the same thing on it; the verb bar's
	// buttons are screen coordinates outright.
	struct ScreenPoint {
		int x, y;
	};
	// The two portrait buttons at the right-hand end of the verb bar. Empty
	// while the bar is not up.
	void collectSwitchButtons(Common::Array<ScreenPoint> &out) const;
	// Whether the verb bar is on screen, which is the game's own statement
	// that it is the player's turn.
	bool interfaceUp() const;
	// The nine verb buttons, in the order they are drawn: left to right, top
	// row first. Fewer than nine means this is not the verb bar.
	void collectVerbGrid(Common::Array<uint> &order,
	                     const Common::Array<Verb> &buttons) const;

	void failCall(const Common::String &what, const Common::String &reason);
	bool toolChooseKids(const Common::JSONValue &args, Common::String &errorOut);
	bool toolSwitchCharacter(const Common::JSONValue &args, Common::String &errorOut);

	Phase _phase;
	Common::Array<KidSlot> _slots;
	// One entry per slot: who the character table says is drawn there, replaced
	// by the name the game itself gives once that portrait has been clicked.
	Common::Array<Common::String> _slotNames;
	Common::Array<bool> _slotHeard;            // has the game named this slot itself?
	Common::Array<Common::String> _wanted;     // asked for, not picked yet
	Common::Array<Common::String> _chosen;     // the team, once it is settled
	Common::Array<Common::String> _cast;       // the seven kids, in portrait order
	int _startX, _startY;
	uint _pickIndex;                           // slot being clicked
	uint32 _phaseFrame;                        // frame the current phase started
	int _selectRoom;                           // the title screen's room
	bool _skipIntro;
	int _escapes;
	uint32 _lastEscapeFrame;

	// switch_character. The pair of buttons is the two kids who are *not* being
	// controlled, so which button is whom changes with every press and cannot
	// be worked out in advance. What can be relied on is that from wherever
	// control is, one of the two buttons is the kid that was asked for - so a
	// press that lands on the wrong kid is not a failure, it is one of the two
	// answers, and the other button is then known to be the right one. This
	// remembers which buttons have been tried from which kid, so no press is
	// ever repeated and the search ends.
	Common::String _switchWanted;
	Common::String _switchFrom;                // who held control when the press went out
	Common::Array<ScreenPoint> _switchButtons;
	Common::Array<Common::String> _switchTriedFrom;   // paired with _switchTriedButton
	Common::Array<uint> _switchTriedButton;
	uint _switchIndex;                         // the button being pressed
};

} // End of namespace AGS3

#endif
