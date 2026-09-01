#include <cxxtest/TestSuite.h>

#include "agi/mcp_names.h"

// The AGI naming helpers (mcp_names.cpp) are deliberately free of any
// AgiEngine dependency, so they link into the cxxtest runner without the
// engine runtime.
//
// AGI names two things and both are words a person typed: the items in the
// OBJECT file and the words in the parser's dictionary. What these helpers do
// is turn prose written for a 1988 player into identifiers an agent can pass
// back, and tell the game's own voice apart from the interpreter's furniture.

class AgiMcpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Item names ---------------------------------------------------------
	void test_words_are_joined_and_lowered() {
		TS_ASSERT_EQUALS(Agi::agiItemName("Chicken Feather"), "chicken_feather");
		TS_ASSERT_EQUALS(Agi::agiItemName("Fish Bone Powder"), "fish_bone_powder");
	}

	// The OBJECT file is prose: "a small chest of gold" is one item, and an
	// agent should not have to reproduce the article to name it.
	void test_a_leading_article_is_dropped() {
		TS_ASSERT_EQUALS(Agi::agiItemName("A Small Chest Of Gold"), "small_chest_of_gold");
		TS_ASSERT_EQUALS(Agi::agiItemName("the key"), "key");
		TS_ASSERT_EQUALS(Agi::agiItemName("an apple"), "apple");
	}

	// Only a *leading* one, though: the "of" in the middle is part of the name.
	void test_an_article_in_the_middle_stays() {
		TS_ASSERT_EQUALS(Agi::agiItemName("chest of the gold"), "chest_of_the_gold");
	}

	// An item that is only an article keeps it rather than becoming nothing:
	// a name that folds away entirely cannot be typed back.
	void test_a_name_that_is_only_an_article_survives() {
		TS_ASSERT_EQUALS(Agi::agiItemName("the"), "the");
	}

	void test_punctuation_is_dropped_and_separators_do_not_double_up() {
		TS_ASSERT_EQUALS(Agi::agiItemName("Thimble & Dew"), "thimble_dew");
		TS_ASSERT_EQUALS(Agi::agiItemName("dagger's edge"), "daggers_edge");
		TS_ASSERT_EQUALS(Agi::agiItemName("rose  essence"), "rose_essence");
		TS_ASSERT_EQUALS(Agi::agiItemName("fly-wings"), "fly_wings");
	}

	void test_a_name_with_nothing_in_it_folds_to_nothing() {
		TS_ASSERT_EQUALS(Agi::agiItemName("   "), "");
		TS_ASSERT_EQUALS(Agi::agiItemName("???"), "");
	}

	// --- Empty slots --------------------------------------------------------
	// The object table is a fixed-length array and its holes are "?", which is
	// a perfectly good target right up until an agent tries to take it.
	void test_the_tables_holes_are_recognised() {
		TS_ASSERT(Agi::agiIsPlaceholderItem("?"));
		TS_ASSERT(Agi::agiIsPlaceholderItem(" ? "));
		TS_ASSERT(Agi::agiIsPlaceholderItem(""));
	}

	void test_a_real_item_is_not_a_hole() {
		TS_ASSERT(!Agi::agiIsPlaceholderItem("Chicken Feather"));
		TS_ASSERT(!Agi::agiIsPlaceholderItem("Thimble*"));
	}

	// --- Disambiguation -----------------------------------------------------
	void test_the_first_of_a_name_keeps_it_and_the_rest_are_numbered() {
		TS_ASSERT_EQUALS(Agi::agiDisambiguate("key", 0), "key");
		TS_ASSERT_EQUALS(Agi::agiDisambiguate("key", 1), "key_2");
		TS_ASSERT_EQUALS(Agi::agiDisambiguate("key", 2), "key_3");
	}

	void test_nothing_is_never_numbered() {
		TS_ASSERT_EQUALS(Agi::agiDisambiguate("", 3), "");
	}

	// --- Messages -----------------------------------------------------------
	// The interpreter hand-wraps its messages, so a sentence arrives as
	// several rows with newlines through the middle of it.
	void test_hand_wrapped_rows_become_one_line() {
		TS_ASSERT_EQUALS(Agi::agiJoinMessage("You see a\nsmall wooden\nchest."),
		                 "You see a small wooden chest.");
	}

	void test_runs_of_space_collapse_and_the_edges_are_trimmed() {
		TS_ASSERT_EQUALS(Agi::agiJoinMessage("  hello   there  "), "hello there");
		TS_ASSERT_EQUALS(Agi::agiJoinMessage("\n\nhello\n\n"), "hello");
	}

	// --- The interpreter's furniture ---------------------------------------
	// A line the interpreter draws is not the game saying anything, and an
	// agent reading the status bar as dialogue is reading the wrong thing.
	void test_the_prompt_and_the_status_line_are_furniture() {
		TS_ASSERT(Agi::agiIsInterfaceLine(">"));
		TS_ASSERT(Agi::agiIsInterfaceLine("> look at chest"));
		TS_ASSERT(Agi::agiIsInterfaceLine("]"));
		TS_ASSERT(Agi::agiIsInterfaceLine("Score:0 of 210    Sound:on"));
		TS_ASSERT(Agi::agiIsInterfaceLine("Press Enter to continue."));
	}

	// King's Quest III draws a running clock every second. Read as game text
	// those are a stopwatch, not a story.
	void test_the_clock_is_furniture() {
		TS_ASSERT(Agi::agiIsInterfaceLine("0:00:07"));
		TS_ASSERT(Agi::agiIsInterfaceLine(" 1:23:45 "));
	}

	void test_a_line_the_game_actually_said_is_kept() {
		TS_ASSERT(!Agi::agiIsInterfaceLine("You see a small wooden chest."));
		TS_ASSERT(!Agi::agiIsInterfaceLine("The three-headed dragon blocks your way."));
	}

	// A number inside a sentence does not make the sentence a clock.
	void test_a_sentence_with_a_number_in_it_is_not_a_clock() {
		TS_ASSERT(!Agi::agiIsInterfaceLine("There are 3 doors here."));
	}
};
