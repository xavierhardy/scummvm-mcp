#include <cxxtest/TestSuite.h>

#include "toon/mcp_names.h"

#include "engines/mcp_bridge.h"

// The Toon naming helpers (mcp_names.cpp) are deliberately free of any
// ToonEngine dependency — like engines/mcp_bridge_text.cpp — so they link into
// the cxxtest runner without the engine runtime. Every name an agent sees
// comes from the game's own data: the line written along the bottom of the
// screen for whatever is being pointed at, the line the player character
// speaks about an item, and the file a scene's data lives in. These functions
// only fold those raw strings into stable identifiers, and supply a fallback
// for the things the game leaves unnamed.

class ToonMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Scene names --------------------------------------------------------
	void test_scene_name_from_data_file() {
		TS_ASSERT_EQUALS(Toon::toonSceneName("ZCROSS"), "zcross");
		TS_ASSERT_EQUALS(Toon::toonSceneName("JIMEX.CPS"), "jimex");
		TS_ASSERT_EQUALS(Toon::toonSceneName("WacExDbl"), "wacexdbl");
	}

	void test_scene_name_drops_any_folder() {
		TS_ASSERT_EQUALS(Toon::toonSceneName("act1/ZCROSS.CPS"), "zcross");
		TS_ASSERT_EQUALS(Toon::toonSceneName("ACT1\\JIMEX"), "jimex");
	}

	void test_scene_name_of_nothing_is_nothing() {
		TS_ASSERT_EQUALS(Toon::toonSceneName(""), "");
		TS_ASSERT_EQUALS(Toon::toonSceneName(".CPS"), "");
	}

	// --- Labels -------------------------------------------------------------
	void test_label_becomes_an_identifier() {
		TS_ASSERT_EQUALS(Toon::toonLabelToName("Zanydu Clock"), "zanydu_clock");
		TS_ASSERT_EQUALS(Toon::toonLabelToName("PATH"), "path");
		TS_ASSERT_EQUALS(Toon::toonLabelToName("Flux Wildly"), "flux_wildly");
	}

	void test_label_punctuation_is_dropped() {
		TS_ASSERT_EQUALS(Toon::toonLabelToName("Drew's hat!"), "drews_hat");
		TS_ASSERT_EQUALS(Toon::toonLabelToName("Wacme  Entrance"), "wacme_entrance");
		TS_ASSERT_EQUALS(Toon::toonLabelToName("half-open door"), "half_open_door");
	}

	void test_label_of_nothing_is_nothing() {
		TS_ASSERT_EQUALS(Toon::toonLabelToName(""), "");
		TS_ASSERT_EQUALS(Toon::toonLabelToName("...!?"), "");
	}

	// --- Fallbacks ----------------------------------------------------------
	void test_fallback_names_say_what_kind_of_thing_it_is() {
		TS_ASSERT_EQUALS(Toon::toonFallbackName("object", 8), "object_8");
		TS_ASSERT_EQUALS(Toon::toonFallbackName("exit", 0), "exit_0");
		TS_ASSERT_EQUALS(Toon::toonFallbackName("item", 59), "item_59");
		TS_ASSERT_EQUALS(Toon::toonFallbackName(nullptr, 3), "thing_3");
	}

	// --- Duplicates ---------------------------------------------------------
	void test_the_first_of_a_name_keeps_it() {
		TS_ASSERT_EQUALS(Toon::toonDisambiguate("path", 0), "path");
		TS_ASSERT_EQUALS(Toon::toonDisambiguate("path", 1), "path_2");
		TS_ASSERT_EQUALS(Toon::toonDisambiguate("path", 2), "path_3");
	}

	// --- Conversation options -----------------------------------------------
	void test_topic_is_the_opening_of_the_line() {
		TS_ASSERT_EQUALS(
		    Toon::toonTopicName("Let me get this straight. You're guarding this outhouse?"),
		    "let_me_get_this_straight");
		TS_ASSERT_EQUALS(Toon::toonTopicName("We have to go, but thanks."),
		                 "we_have_to_go_but");
	}

	void test_topic_keeps_a_short_line_whole() {
		TS_ASSERT_EQUALS(Toon::toonTopicName("Hello there!"), "hello_there");
		TS_ASSERT_EQUALS(Toon::toonTopicName("Bye."), "bye");
	}

	void test_topic_word_count_is_the_caller_s() {
		TS_ASSERT_EQUALS(Toon::toonTopicName("one two three four five six", 3),
		                 "one_two_three");
		TS_ASSERT_EQUALS(Toon::toonTopicName("one two three four five six", 1), "one");
	}

	void test_topic_of_nothing_is_nothing() {
		TS_ASSERT_EQUALS(Toon::toonTopicName(""), "");
		TS_ASSERT_EQUALS(Toon::toonTopicName("?!..."), "");
	}

	// --- Items --------------------------------------------------------------
	void test_item_name_is_what_the_description_names() {
		TS_ASSERT_EQUALS(Toon::toonItemName("A plunger."), "plunger");
		TS_ASSERT_EQUALS(Toon::toonItemName("A herring."), "herring");
		TS_ASSERT_EQUALS(Toon::toonItemName("One stick of Marge's butter."),
		                 "stick_of_marges_butter");
	}

	void test_item_name_drops_only_the_words_that_open_the_sentence() {
		TS_ASSERT_EQUALS(Toon::toonItemName("It's the rubber chicken."),
		                 "rubber_chicken");
		// "of" and "the" in the middle are part of what it is called.
		TS_ASSERT_EQUALS(Toon::toonItemName("A can of the good stuff"),
		                 "can_of_the_good");
	}

	void test_item_name_word_count_is_the_caller_s() {
		TS_ASSERT_EQUALS(Toon::toonItemName("A big red rubber chicken", 2), "big_red");
	}

	void test_item_name_of_lead_ins_alone_keeps_them() {
		// Better a weak name than none at all: an unnameable item could not be
		// referred to.
		TS_ASSERT_EQUALS(Toon::toonItemName("It's a thing", 4), "thing");
		TS_ASSERT_EQUALS(Toon::toonItemName("The one"), "the_one");
	}

	void test_item_name_of_nothing_is_nothing() {
		TS_ASSERT_EQUALS(Toon::toonItemName(""), "");
		TS_ASSERT_EQUALS(Toon::toonItemName("!?"), "");
	}
};
