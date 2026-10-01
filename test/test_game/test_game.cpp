// Tests of the game rules.
//
// The rules themselves keep their German identifiers, because they are a port
// and should still line up with sketches/stufe2-zaehlwerk-mvp line for line.
// The test names are prose that ends up in the test output, so they are English
// like everything else a developer reads here.

#include <unity.h>

#include <string>

#include "game.h"

void setUp() {}
void tearDown() {}

/* ---------- serve rotation ---------- */

static void the_serve_changes_after_every_second_point() {
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(0, 0, 'A'));
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(1, 0, 'A'));
  TEST_ASSERT_EQUAL_CHAR('B', game::aufschlagFuer(1, 1, 'A'));
  TEST_ASSERT_EQUAL_CHAR('B', game::aufschlagFuer(2, 1, 'A'));
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(2, 2, 'A'));
}

static void from_ten_all_the_serve_changes_after_every_point() {
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(10, 10, 'A'));
  TEST_ASSERT_EQUAL_CHAR('B', game::aufschlagFuer(11, 10, 'A'));
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(11, 11, 'A'));
}

static void who_serves_first_flips_the_whole_sequence() {
  TEST_ASSERT_EQUAL_CHAR('B', game::aufschlagFuer(0, 0, 'B'));
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(1, 1, 'B'));
}

/* ---------- end of a set ---------- */

static void eleven_points_with_two_clear_ends_the_set() {
  TEST_ASSERT_TRUE(game::beendet(11, 9));
  TEST_ASSERT_TRUE(game::beendet(9, 11));
  TEST_ASSERT_FALSE(game::beendet(10, 9));
}

static void a_single_point_lead_carries_on() {
  TEST_ASSERT_FALSE(game::beendet(11, 10));
  TEST_ASSERT_TRUE(game::beendet(12, 10));
  TEST_ASSERT_FALSE(game::beendet(15, 14));
}

/* ---------- reading a rally ---------- */

static void the_point_goes_to_the_side_of_the_second_to_last_bounce() {
  game::Urteil u = game::rallyBewerten("ABAB", 'A');
  TEST_ASSERT_EQUAL_CHAR('A', u.gewinner);
  TEST_ASSERT_TRUE(u.hinweis.empty());
}

static void a_single_bounce_awards_nothing() {
  // Fired five times across two games on 2026-10-01 and four were taken back
  // within seconds. The sequence is recorded, the point is given by hand.
  game::Urteil u = game::rallyBewerten("A", 'A');
  TEST_ASSERT_EQUAL_CHAR(' ', u.gewinner);
  TEST_ASSERT_EQUAL_STRING("single_bounce", u.grund);
  TEST_ASSERT_TRUE(u.hinweis.find("Nur ein Aufsetzer") != std::string::npos);
}

// The rule beside it still awards: four times in the same two games, never
// corrected. Switching both off together was the mistake the evening before.
static void an_unclear_serve_still_awards() {
  game::Urteil u = game::rallyBewerten("AB", 'A');
  TEST_ASSERT_EQUAL_CHAR('A', u.gewinner);
  TEST_ASSERT_EQUAL_STRING("serve_unclear", u.grund);
}

static void two_bounces_from_the_serve_are_ambiguous() {
  // An ace and a serve that clipped the net are the same two bounces from under
  // the table, and they are not the same thing: one is a point, the other a let.
  game::Urteil u = game::rallyBewerten("AB", 'A');
  TEST_ASSERT_EQUAL_CHAR('A', u.gewinner);
  TEST_ASSERT_EQUAL_STRING("serve_unclear", u.grund);
  TEST_ASSERT_TRUE(u.hinweis.find("Ass oder Netzaufschlag") != std::string::npos);
}

// The rules that still award, so the change above cannot quietly swallow them.
static void a_longer_rally_still_awards() {
  game::Urteil u = game::rallyBewerten("ABAB", 'A');
  TEST_ASSERT_EQUAL_CHAR('A', u.gewinner);
  TEST_ASSERT_EQUAL_STRING("last_bounce", u.grund);
}

static void a_double_bounce_still_awards() {
  game::Urteil u = game::rallyBewerten("ABB", 'A');
  TEST_ASSERT_EQUAL_CHAR('A', u.gewinner);
  TEST_ASSERT_EQUAL_STRING("double_bounce", u.grund);
}

static void a_first_bounce_on_the_wrong_side_is_recorded() {
  game::Urteil u = game::rallyBewerten("BABA", 'A');
  TEST_ASSERT_EQUAL_CHAR('B', u.gewinner);
  TEST_ASSERT_TRUE(u.hinweis.find("nicht auf der Aufschlagseite") != std::string::npos);
}

static void an_empty_sequence_awards_nothing() {
  game::Urteil u = game::rallyBewerten("", 'A');
  TEST_ASSERT_EQUAL_CHAR(' ', u.gewinner);
  TEST_ASSERT_EQUAL_STRING("none", u.grund);
}

// The reason is what the log records, so it has to say which rule fired rather
// than being derived from the German hint afterwards.
static void the_reason_names_the_rule_that_decided() {
  TEST_ASSERT_EQUAL_STRING("last_bounce", game::rallyBewerten("ABAB", 'A').grund);
  TEST_ASSERT_EQUAL_STRING("single_bounce", game::rallyBewerten("A", 'A').grund);
  TEST_ASSERT_EQUAL_STRING("double_bounce", game::rallyBewerten("ABAA", 'A').grund);
  // A first bounce on the wrong side is a hint, not a rule — the last bounce
  // still decides.
  TEST_ASSERT_EQUAL_STRING("last_bounce", game::rallyBewerten("BABA", 'A').grund);
}

// Two bounces in a row on the same half mean the ball did not come back, so the
// point belongs to the other side.
static void a_double_bounce_is_recognised_as_one() {
  game::Urteil u = game::rallyBewerten("ABAA", 'A');
  TEST_ASSERT_TRUE(u.hinweis.find("Doppelaufsetzer") != std::string::npos);
}

static void the_point_goes_to_the_side_that_did_not_double_bounce() {
  // A double bounce on A at the end — A did not get it back.
  TEST_ASSERT_EQUAL_CHAR('B', game::rallyBewerten("ABAA", 'A').gewinner);
  // The same on B.
  TEST_ASSERT_EQUAL_CHAR('A', game::rallyBewerten("BABB", 'B').gewinner);
}

// The case the branch exists for: play continued after the second bounce. The
// rally was over before that, so the last bounce must not decide any more.
static void a_double_bounce_decides_even_when_play_continued() {
  // "ABBA": double bounce on B, point to A. By the last bounce alone it would
  // be 'B' — here the double bounce wins.
  TEST_ASSERT_EQUAL_CHAR('A', game::rallyBewerten("ABBA", 'A').gewinner);
  // "BAAB": double bounce on A, point to B; by the last bounce it would be 'A'.
  TEST_ASSERT_EQUAL_CHAR('B', game::rallyBewerten("BAAB", 'B').gewinner);
}

// The first double bounce decides, not the last — after it the rally is over and
// everything further is measurement noise.
static void the_first_double_bounce_decides() {
  TEST_ASSERT_EQUAL_CHAR('B', game::rallyBewerten("AABB", 'A').gewinner);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(the_serve_changes_after_every_second_point);
  RUN_TEST(from_ten_all_the_serve_changes_after_every_point);
  RUN_TEST(who_serves_first_flips_the_whole_sequence);
  RUN_TEST(eleven_points_with_two_clear_ends_the_set);
  RUN_TEST(a_single_point_lead_carries_on);
  RUN_TEST(the_point_goes_to_the_side_of_the_second_to_last_bounce);
  RUN_TEST(a_single_bounce_awards_nothing);
  RUN_TEST(an_unclear_serve_still_awards);
  RUN_TEST(two_bounces_from_the_serve_are_ambiguous);
  RUN_TEST(a_longer_rally_still_awards);
  RUN_TEST(a_double_bounce_still_awards);
  RUN_TEST(a_first_bounce_on_the_wrong_side_is_recorded);
  RUN_TEST(an_empty_sequence_awards_nothing);
  RUN_TEST(the_reason_names_the_rule_that_decided);
  RUN_TEST(a_double_bounce_is_recognised_as_one);
  RUN_TEST(the_point_goes_to_the_side_that_did_not_double_bounce);
  RUN_TEST(a_double_bounce_decides_even_when_play_continued);
  RUN_TEST(the_first_double_bounce_decides);
  return UNITY_END();
}
