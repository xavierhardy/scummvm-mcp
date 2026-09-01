#include <cxxtest/TestSuite.h>

#include "ags/mcp_names.h"

#include "engines/mcp_bridge.h"

// The AGS naming helpers (mcp_names.cpp) are deliberately free of any engine
// dependency — like engines/mcp_bridge_text.cpp — so they link into the
// cxxtest runner without the engine runtime.
//
// AGS is a toolkit, so everything in a room was named by its author at design
// time and both names survive to run time: a display name meant for a player
// ("Front Door") and a script name meant for the game's code ("oFrontDoor").
// These fold both into one identifier, prefer the one the game shows a player,
// and know an editor default when they see one.

class AgsMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Display names ------------------------------------------------------
	void test_a_display_name_becomes_an_identifier() {
		TS_ASSERT_EQUALS(AGS3::agsDisplayName("Front Door"), "front_door");
		TS_ASSERT_EQUALS(AGS3::agsDisplayName("Cat Clock"), "cat_clock");
		TS_ASSERT_EQUALS(AGS3::agsDisplayName("bed"), "bed");
	}

	// An apostrophe joins rather than separates: "Zak's" is one word, not two.
	void test_an_apostrophe_does_not_split_a_word() {
		TS_ASSERT_EQUALS(AGS3::agsDisplayName("Zak's bed"), "zaks_bed");
		TS_ASSERT_EQUALS(AGS3::agsDisplayName("Drew's hat!"), "drews_hat");
	}

	void test_camel_humps_split_but_an_all_caps_run_does_not() {
		TS_ASSERT_EQUALS(AGS3::agsDisplayName("FrontDoor"), "front_door");
		TS_ASSERT_EQUALS(AGS3::agsDisplayName("TV"), "tv");
	}

	void test_a_name_of_nothing_is_nothing() {
		TS_ASSERT_EQUALS(AGS3::agsDisplayName(""), "");
		TS_ASSERT_EQUALS(AGS3::agsDisplayName("!!!"), "");
	}

	// --- Script names -------------------------------------------------------
	// The AGS convention puts one letter of type in front of a capitalised
	// name: o for a room object, h for a hotspot, c for a character, i for an
	// inventory item.
	void test_the_authors_type_prefix_comes_off() {
		TS_ASSERT_EQUALS(AGS3::agsScriptName("oFrontDoor"), "front_door");
		TS_ASSERT_EQUALS(AGS3::agsScriptName("hDoor"), "door");
		TS_ASSERT_EQUALS(AGS3::agsScriptName("cZak"), "zak");
		TS_ASSERT_EQUALS(AGS3::agsScriptName("iKey"), "key");
	}

	// ...but only when that is really the shape. A script name that is simply
	// a lower-case word keeps all of itself.
	void test_a_plain_word_keeps_its_first_letter() {
		TS_ASSERT_EQUALS(AGS3::agsScriptName("door"), "door");
		TS_ASSERT_EQUALS(AGS3::agsScriptName("clock"), "clock");
		TS_ASSERT_EQUALS(AGS3::agsScriptName("output"), "output");
	}

	// --- Which name to publish ----------------------------------------------
	// The display name is what the game calls the thing when it talks to a
	// player, so it is what an agent reading the game's own words recognises.
	void test_the_name_shown_to_a_player_wins() {
		TS_ASSERT_EQUALS(AGS3::agsThingName("Front Door", "oFrontDoor"), "front_door");
		TS_ASSERT_EQUALS(AGS3::agsThingName("Cat Clock", "oClock"), "cat_clock");
	}

	void test_the_script_name_is_the_fallback() {
		TS_ASSERT_EQUALS(AGS3::agsThingName("", "oFrontDoor"), "front_door");
		TS_ASSERT_EQUALS(AGS3::agsThingName("", "cZak"), "zak");
	}

	void test_nameless_is_nameless() {
		TS_ASSERT_EQUALS(AGS3::agsThingName("", ""), "");
	}

	// --- Editor defaults ----------------------------------------------------
	// A hotspot nobody named is still called "Hotspot 4" in the data. Offering
	// that is offering a number dressed as a name.
	void test_an_editor_default_is_not_a_name() {
		TS_ASSERT(AGS3::agsIsPlaceholderName("Hotspot 4", 4));
		TS_ASSERT(AGS3::agsIsPlaceholderName("Object 2", 2));
		TS_ASSERT(AGS3::agsIsPlaceholderName("Character 7", 7));
		TS_ASSERT(AGS3::agsIsPlaceholderName("", 1));
	}

	// The number has to match: "Hotspot 4" at index 9 was typed by someone.
	void test_a_default_belonging_to_another_index_is_a_real_name() {
		TS_ASSERT(!AGS3::agsIsPlaceholderName("Hotspot 4", 9));
	}

	void test_a_real_name_is_kept() {
		TS_ASSERT(!AGS3::agsIsPlaceholderName("Front Door", 3));
		TS_ASSERT(!AGS3::agsIsPlaceholderName("bed", 1));
	}

	// --- Fallbacks and duplicates -------------------------------------------
	void test_something_with_no_name_can_still_be_reached() {
		TS_ASSERT_EQUALS(AGS3::agsFallbackName("object", 7), "object_7");
		TS_ASSERT_EQUALS(AGS3::agsFallbackName("character", 0), "character_0");
	}

	void test_the_first_of_a_name_keeps_it_and_the_rest_are_numbered() {
		TS_ASSERT_EQUALS(AGS3::agsDisambiguate("door", 0), "door");
		TS_ASSERT_EQUALS(AGS3::agsDisambiguate("door", 1), "door_2");
		TS_ASSERT_EQUALS(AGS3::agsDisambiguate("door", 2), "door_3");
	}
};
