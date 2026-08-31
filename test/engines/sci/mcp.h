#include <cxxtest/TestSuite.h>

#include "sci/mcp_names.h"

#include "engines/mcp_bridge.h"

// The SCI naming helpers (mcp_names.cpp) are deliberately free of any
// SciEngine dependency — like engines/mcp_bridge_text.cpp — so they link into
// the cxxtest runner without the engine runtime.
//
// SCI is unlike the pointer games the other bridges cover: it does not label
// what the player points at, so there is no painted string to fold. What it
// has instead is a script-level name on every object, the identifier the
// game's own author typed, carried in the object header. These functions turn
// an author's identifier into an agent's, and decide which of them are things
// in the room at all.

class SciMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Object names -------------------------------------------------------
	void test_camel_humps_become_words() {
		TS_ASSERT_EQUALS(Sci::sciObjectName("gateDoor"), "gate_door");
		TS_ASSERT_EQUALS(Sci::sciObjectName("magnifyingGlass"), "magnifying_glass");
		TS_ASSERT_EQUALS(Sci::sciObjectName("coffeePot"), "coffee_pot");
	}

	void test_a_name_already_in_one_word_is_left_alone() {
		TS_ASSERT_EQUALS(Sci::sciObjectName("painting"), "painting");
		TS_ASSERT_EQUALS(Sci::sciObjectName("tweezers"), "tweezers");
	}

	// An all-caps run is one word, not one word per letter: a game that calls
	// something GK1Door means one door, not a g and a k and a door.
	void test_an_all_caps_run_stays_one_word() {
		TS_ASSERT_EQUALS(Sci::sciObjectName("GK1Door"), "gk1_door");
		TS_ASSERT_EQUALS(Sci::sciObjectName("SQ6"), "sq6");
	}

	void test_punctuation_separates_and_never_doubles_up() {
		TS_ASSERT_EQUALS(Sci::sciObjectName("shop-door"), "shop_door");
		TS_ASSERT_EQUALS(Sci::sciObjectName("shop  door"), "shop_door");
		TS_ASSERT_EQUALS(Sci::sciObjectName("shop__door"), "shop_door");
		TS_ASSERT_EQUALS(Sci::sciObjectName("/room210/door"), "room210_door");
	}

	// "theShovel" and "shovel" are the same thing said two ways, and an agent
	// should not have to know which one this game's author preferred.
	void test_a_leading_the_is_dropped() {
		TS_ASSERT_EQUALS(Sci::sciObjectName("theShovel"), "shovel");
		TS_ASSERT_EQUALS(Sci::sciObjectName("theBook"), "book");
	}

	// ...but only when there is something left to call it.
	void test_a_name_that_is_only_the_keeps_it() {
		TS_ASSERT_EQUALS(Sci::sciObjectName("the"), "the");
		TS_ASSERT_EQUALS(Sci::sciObjectName("theme"), "theme");
	}

	void test_a_name_of_nothing_is_nothing() {
		TS_ASSERT_EQUALS(Sci::sciObjectName(""), "");
		TS_ASSERT_EQUALS(Sci::sciObjectName("!!!"), "");
	}

	// --- What is bookkeeping and what is in the room ------------------------
	void test_the_interpreters_own_objects_are_not_things_to_act_on() {
		TS_ASSERT(Sci::sciIsInternalName("aMover"));
		TS_ASSERT(Sci::sciIsInternalName("aPolyPath"));
		TS_ASSERT(Sci::sciIsInternalName("cycler"));
		TS_ASSERT(Sci::sciIsInternalName("aTimer"));
		TS_ASSERT(Sci::sciIsInternalName("theGame"));
	}

	// Screen furniture a game draws with sits in the same cast as the actors.
	void test_text_and_border_objects_are_not_things_to_act_on() {
		TS_ASSERT(Sci::sciIsInternalName("dtext"));
		TS_ASSERT(Sci::sciIsInternalName("dtext2"));
		TS_ASSERT(Sci::sciIsInternalName("bord"));
	}

	void test_things_in_the_room_are_kept() {
		TS_ASSERT(!Sci::sciIsInternalName("coffeePot"));
		TS_ASSERT(!Sci::sciIsInternalName("shopDoor"));
		TS_ASSERT(!Sci::sciIsInternalName("captain"));
		TS_ASSERT(!Sci::sciIsInternalName("gkEgo"));
	}

	// "sound" alone is the sound handle; a soundBox is a box.
	void test_a_bookkeeping_word_only_counts_when_it_is_the_whole_name() {
		TS_ASSERT(Sci::sciIsInternalName("sound"));
		TS_ASSERT(Sci::sciIsInternalName("music"));
		TS_ASSERT(!Sci::sciIsInternalName("musicBox"));
	}

	void test_a_nameless_object_is_not_something_to_act_on() {
		TS_ASSERT(Sci::sciIsInternalName(""));
	}

	// --- Fallbacks and duplicates -------------------------------------------
	void test_something_the_script_never_named_can_still_be_reached() {
		TS_ASSERT_EQUALS(Sci::sciFallbackName("object", 7), "object_7");
		TS_ASSERT_EQUALS(Sci::sciFallbackName("item", 0), "item_0");
	}

	void test_the_first_of_a_name_keeps_it_and_the_rest_are_numbered() {
		TS_ASSERT_EQUALS(Sci::sciDisambiguate("door", 0), "door");
		TS_ASSERT_EQUALS(Sci::sciDisambiguate("door", 1), "door_2");
		TS_ASSERT_EQUALS(Sci::sciDisambiguate("door", 2), "door_3");
	}

	// --- Room names ---------------------------------------------------------
	// SCI rooms are numbered, not named, so this only ever accompanies the
	// number - but the room object's name is usually the more readable half.
	void test_a_room_is_named_after_its_room_object() {
		TS_ASSERT_EQUALS(Sci::sciRoomName("bookstore"), "bookstore");
		TS_ASSERT_EQUALS(Sci::sciRoomName("hansGate"), "hans_gate");
		TS_ASSERT_EQUALS(Sci::sciRoomName(""), "");
	}
};
