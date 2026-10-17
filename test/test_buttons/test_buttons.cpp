// Tests of the button input path: how long a press was, and what it does.

#include <unity.h>


#include "buttons.h"

using buttons::Action;
using buttons::Gesture;
using buttons::Source;

void setUp() {}
void tearDown() {}

static buttons::Event press(char side, Gesture g) {
  return {side, g, Source::Virtual, 0};
}

static const buttons::Board PLAYING{false, false};
static const buttons::Board OVER{true, false};
static const buttons::Board LINKED{false, true};
static const buttons::Board LINKED_OVER{true, true};

/* ---------- timing ---------- */

static void a_press_just_under_the_long_threshold_is_short() {
  const buttons::Timing t;
  TEST_ASSERT_EQUAL(Gesture::Short, buttons::classify(0, t));
  TEST_ASSERT_EQUAL(Gesture::Short, buttons::classify(999, t));
}

static void a_press_at_the_long_threshold_is_long() {
  const buttons::Timing t;
  TEST_ASSERT_EQUAL(Gesture::Long, buttons::classify(1000, t));
  TEST_ASSERT_EQUAL(Gesture::Long, buttons::classify(2999, t));
}

static void a_press_at_the_very_long_threshold_is_very_long() {
  const buttons::Timing t;
  TEST_ASSERT_EQUAL(Gesture::VeryLong, buttons::classify(3000, t));
  TEST_ASSERT_EQUAL(Gesture::VeryLong, buttons::classify(60000, t));
}

static void the_thresholds_are_the_ones_that_are_set() {
  buttons::Timing t;
  t.longMs = 600;
  t.veryLongMs = 2000;
  TEST_ASSERT_EQUAL(Gesture::Short, buttons::classify(599, t));
  TEST_ASSERT_EQUAL(Gesture::Long, buttons::classify(600, t));
  TEST_ASSERT_EQUAL(Gesture::VeryLong, buttons::classify(2000, t));
}

static void a_timing_nobody_can_press_is_not_valid() {
  TEST_ASSERT_TRUE(buttons::valid(buttons::Timing{}));
  TEST_ASSERT_FALSE(buttons::valid(buttons::Timing{100, 3000}));    // long is a tap
  TEST_ASSERT_FALSE(buttons::valid(buttons::Timing{1000, 1200}));   // too close to long
  TEST_ASSERT_FALSE(buttons::valid(buttons::Timing{3000, 1000}));   // the wrong way round
  TEST_ASSERT_FALSE(buttons::valid(buttons::Timing{1000, 20000}));  // nobody holds that long
}

/* ---------- what a press does ---------- */

static void a_short_press_is_a_point_for_its_half() {
  const buttons::Decision a = buttons::decide(press('A', Gesture::Short), PLAYING);
  TEST_ASSERT_EQUAL(Action::Point, a.action);
  TEST_ASSERT_EQUAL(Action::Point, buttons::decide(press('B', Gesture::Short), LINKED).action);
}

static void a_short_press_after_the_game_starts_the_next() {
  TEST_ASSERT_EQUAL(Action::StartGame, buttons::decide(press('B', Gesture::Short), OVER).action);
}

static void a_long_press_takes_back_the_last_point_at_once() {
  TEST_ASSERT_EQUAL(Action::Undo, buttons::decide(press('A', Gesture::Long), PLAYING).action);
  TEST_ASSERT_EQUAL(Action::Undo, buttons::decide(press('A', Gesture::Long), LINKED).action);
}

static void a_long_press_after_the_game_can_still_take_back_the_match_point() {
  TEST_ASSERT_EQUAL(Action::Undo, buttons::decide(press('B', Gesture::Long), OVER).action);
  TEST_ASSERT_EQUAL(Action::Undo, buttons::decide(press('B', Gesture::Long), LINKED_OVER).action);
}

static void a_very_long_press_starts_a_new_game() {
  TEST_ASSERT_EQUAL(Action::NewGame, buttons::decide(press('A', Gesture::VeryLong), PLAYING).action);
  TEST_ASSERT_EQUAL(Action::NewGame, buttons::decide(press('B', Gesture::VeryLong), OVER).action);
}

static void while_linked_no_button_starts_a_game() {
  const buttons::Decision v = buttons::decide(press('A', Gesture::VeryLong), LINKED);
  TEST_ASSERT_EQUAL(Action::Refused, v.action);
  TEST_ASSERT_EQUAL_STRING("next_game_on_zaehlwerk", v.reason);
  const buttons::Decision s = buttons::decide(press('A', Gesture::Short), LINKED_OVER);
  TEST_ASSERT_EQUAL(Action::Refused, s.action);
  TEST_ASSERT_EQUAL_STRING("next_game_on_zaehlwerk", s.reason);
}

static void a_point_needs_a_half() {
  const buttons::Decision d = buttons::decide(press(' ', Gesture::Short), PLAYING);
  TEST_ASSERT_EQUAL(Action::Refused, d.action);
  TEST_ASSERT_EQUAL_STRING("no_side", d.reason);
}

// The page's "take back" and "new game" belong to no half.
static void taking_back_and_starting_over_need_no_half() {
  TEST_ASSERT_EQUAL(Action::Undo, buttons::decide(press(' ', Gesture::Long), PLAYING).action);
  TEST_ASSERT_EQUAL(Action::NewGame, buttons::decide(press(' ', Gesture::VeryLong), PLAYING).action);
}

static void only_a_refusal_carries_a_reason() {
  TEST_ASSERT_EQUAL_STRING("", buttons::decide(press('A', Gesture::Short), PLAYING).reason);
}

/* ---------- names in the log ---------- */

static void the_names_are_the_ones_the_log_uses() {
  TEST_ASSERT_EQUAL_STRING("short", buttons::gestureName(Gesture::Short));
  TEST_ASSERT_EQUAL_STRING("long", buttons::gestureName(Gesture::Long));
  TEST_ASSERT_EQUAL_STRING("very_long", buttons::gestureName(Gesture::VeryLong));
  TEST_ASSERT_EQUAL_STRING("virtual", buttons::sourceName(Source::Virtual));
  TEST_ASSERT_EQUAL_STRING("espnow", buttons::sourceName(Source::EspNow));
  TEST_ASSERT_EQUAL_STRING("wired", buttons::sourceName(Source::Wired));
  TEST_ASSERT_EQUAL_STRING("new_game", buttons::actionName(Action::NewGame));
  TEST_ASSERT_EQUAL_STRING("start_game", buttons::actionName(Action::StartGame));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(a_press_just_under_the_long_threshold_is_short);
  RUN_TEST(a_press_at_the_long_threshold_is_long);
  RUN_TEST(a_press_at_the_very_long_threshold_is_very_long);
  RUN_TEST(the_thresholds_are_the_ones_that_are_set);
  RUN_TEST(a_timing_nobody_can_press_is_not_valid);
  RUN_TEST(a_short_press_is_a_point_for_its_half);
  RUN_TEST(a_short_press_after_the_game_starts_the_next);
  RUN_TEST(a_long_press_takes_back_the_last_point_at_once);
  RUN_TEST(a_long_press_after_the_game_can_still_take_back_the_match_point);
  RUN_TEST(a_very_long_press_starts_a_new_game);
  RUN_TEST(while_linked_no_button_starts_a_game);
  RUN_TEST(a_point_needs_a_half);
  RUN_TEST(taking_back_and_starting_over_need_no_half);
  RUN_TEST(only_a_refusal_carries_a_reason);
  RUN_TEST(the_names_are_the_ones_the_log_uses);
  return UNITY_END();
}
