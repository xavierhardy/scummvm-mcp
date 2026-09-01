#include <cxxtest/TestSuite.h>

#include "agos/mcp_names.h"

// The AGOS naming helpers (mcp_names.cpp) are deliberately free of any
// engine dependency, so they link into the cxxtest runner without the engine
// runtime.
//
// AGOS is the friendliest of these engines to name things from: every
// clickable thing carries the name the game writes along the bottom of the
// screen. These fold that label into an identifier, and map the words a tool
// caller would reach for onto the twelve buttons the game actually has.

class AgosMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Object names -------------------------------------------------------
	void test_the_shown_name_becomes_an_identifier() {
		TS_ASSERT_EQUALS(AGOS::agosObjectName("a large wooden door"), "large_wooden_door");
		TS_ASSERT_EQUALS(AGOS::agosObjectName("the sign"), "sign");
		TS_ASSERT_EQUALS(AGOS::agosObjectName("Simon"), "simon");
	}

	void test_an_article_in_the_middle_stays() {
		TS_ASSERT_EQUALS(AGOS::agosObjectName("a hole in the wall"), "hole_in_the_wall");
	}

	void test_punctuation_is_dropped_and_separators_do_not_double_up() {
		TS_ASSERT_EQUALS(AGOS::agosObjectName("wizard's hat"), "wizards_hat");
		TS_ASSERT_EQUALS(AGOS::agosObjectName("half-eaten  pie"), "half_eaten_pie");
	}

	void test_a_name_with_nothing_in_it_folds_to_nothing() {
		TS_ASSERT_EQUALS(AGOS::agosObjectName(""), "");
		TS_ASSERT_EQUALS(AGOS::agosObjectName(" -- "), "");
	}

	// --- Disambiguation -----------------------------------------------------
	void test_the_first_of_a_name_keeps_it_and_the_rest_are_numbered() {
		TS_ASSERT_EQUALS(AGOS::agosDisambiguate("door", 0), "door");
		TS_ASSERT_EQUALS(AGOS::agosDisambiguate("door", 1), "door_2");
	}

	// --- The verb bar -------------------------------------------------------
	// Twelve buttons, in the order the engine keeps them. The index is what
	// the engine's own hit areas are numbered by, so these must not drift.
	void test_the_bar_is_twelve_verbs_in_the_engines_order() {
		TS_ASSERT_EQUALS(AGOS::agosVerbCount(), 12);
		TS_ASSERT_EQUALS(AGOS::agosVerbName(0), "walk_to");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(1), "look_at");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(2), "open");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(3), "move");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(4), "consume");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(5), "take");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(6), "close");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(7), "use");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(8), "talk_to");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(9), "remove");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(10), "wear");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(11), "give");
	}

	void test_there_is_no_thirteenth_button() {
		TS_ASSERT_EQUALS(AGOS::agosVerbName(12), "");
		TS_ASSERT_EQUALS(AGOS::agosVerbName(-1), "");
	}

	void test_a_verb_is_found_by_its_own_name() {
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("walk_to"), 0);
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("use"), 7);
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("give"), 11);
	}

	// The words a caller would reach for are not always the words on the bar.
	void test_the_obvious_synonyms_find_the_right_button() {
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("examine"), 1);
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("look"), 1);
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("pick_up"), 5);
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("get"), 5);
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("eat"), 4);
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("speak_to"), 8);
	}

	void test_a_word_the_bar_has_no_button_for_is_refused() {
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex("dance"), -1);
		TS_ASSERT_EQUALS(AGOS::agosVerbIndex(""), -1);
	}
};
