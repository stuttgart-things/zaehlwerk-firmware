// Tests of the check-in rules: the window, which press counts, what an answer
// means, and that no line meant for a log can carry the card number.

#include <unity.h>

#include <string>

#include "checkin.h"

using checkin::Lights;
using checkin::Request;
using checkin::Signal;
using checkin::Uid;
using checkin::Window;

// Compared by name, so a failure says which signal it was rather than a number.
#define ASSERT_SIGNAL(expected, actual) \
  TEST_ASSERT_EQUAL_STRING(checkin::signalName(expected), checkin::signalName(actual))

void setUp() {}
void tearDown() {}

static const uint32_t WINDOW = 5000;

static Uid fob() {
  Uid u{};
  u.bytes[0] = 0x04;
  u.bytes[1] = 0xA3;
  u.bytes[2] = 0x1B;
  u.bytes[3] = 0x22;
  u.size = 4;
  return u;
}

static bool contains(const std::string &s, const char *needle) {
  return s.find(needle) != std::string::npos;
}

static bool holdsNoUid(const Uid &u) {
  if (u.size) return false;
  for (uint8_t b : u.bytes)
    if (b) return false;
  return true;
}

/* ---------- the window ---------- */

static void a_fob_then_a_press_is_a_check_in_at_that_half() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 1000);
  TEST_ASSERT_TRUE(w.waiting());
  ASSERT_SIGNAL(Signal::Waiting, w.signal(1000));

  TEST_ASSERT_TRUE(w.press('B', 3000));
  TEST_ASSERT_TRUE(w.ready());
  ASSERT_SIGNAL(Signal::Sending, w.signal(3000));

  Request r = w.take();
  TEST_ASSERT_EQUAL_CHAR('B', r.half);
  TEST_ASSERT_EQUAL_UINT8(4, r.uid.size);
  TEST_ASSERT_EQUAL_HEX8(0xA3, r.uid.bytes[1]);
}

static void a_press_with_no_fob_is_not_a_check_in() {
  Window w(WINDOW);
  TEST_ASSERT_FALSE(w.press('A', 1000));
  TEST_ASSERT_FALSE(w.ready());
}

static void only_the_first_press_is_taken() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 0);
  TEST_ASSERT_TRUE(w.press('A', 100));
  // The other player's press a moment later is a press, not a second check-in
  // with the same card.
  TEST_ASSERT_FALSE(w.press('B', 200));
  w.take();
  TEST_ASSERT_FALSE(w.press('B', 300));
}

static void no_press_within_the_window_drops_the_fob() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 1000);
  TEST_ASSERT_FALSE(w.expire(1000 + WINDOW - 1));
  TEST_ASSERT_TRUE(w.expire(1000 + WINDOW));
  TEST_ASSERT_FALSE(w.expire(1000 + WINDOW + 1));  // said once, not every tick
  ASSERT_SIGNAL(Signal::Expired, w.signal(1000 + WINDOW));
  TEST_ASSERT_FALSE(w.press('A', 1000 + WINDOW + 10));
  TEST_ASSERT_FALSE(w.ready());
}

static void a_late_press_is_not_a_check_in_even_before_the_expiry_is_noticed() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 1000);
  TEST_ASSERT_FALSE(w.press('A', 1000 + WINDOW));
  TEST_ASSERT_FALSE(w.ready());
  ASSERT_SIGNAL(Signal::Expired, w.signal(1000 + WINDOW));
}

static void the_window_survives_the_millis_wrap() {
  Window w(WINDOW);
  const uint32_t nearWrap = 0xFFFFFFFFu - 1000;
  w.fob(fob(), false, true, nearWrap);
  TEST_ASSERT_FALSE(w.expire(nearWrap + 2000));  // wrapped past zero
  TEST_ASSERT_TRUE(w.press('A', nearWrap + 2000));
}

static void a_second_fob_replaces_the_first_and_starts_the_window_again() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 0);
  Uid other = fob();
  other.bytes[3] = 0x99;
  w.fob(other, false, true, 4000);
  TEST_ASSERT_FALSE(w.expire(WINDOW + 100));  // the first window would be over
  TEST_ASSERT_TRUE(w.press('A', WINDOW + 100));
  TEST_ASSERT_EQUAL_HEX8(0x99, w.take().uid.bytes[3]);
}

static void a_fob_while_a_request_is_out_changes_nothing() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 0);
  w.press('A', 100);
  w.take();
  w.fob(fob(), false, true, 200);
  TEST_ASSERT_FALSE(w.waiting());
  TEST_ASSERT_FALSE(w.press('B', 300));
  ASSERT_SIGNAL(Signal::Sending, w.signal(300));
}

static void a_fob_during_play_does_nothing_and_the_press_stays_a_point() {
  Window w(WINDOW);
  w.fob(fob(), true, true, 0);
  TEST_ASSERT_FALSE(w.waiting());
  ASSERT_SIGNAL(Signal::Rejected, w.signal(0));
  TEST_ASSERT_FALSE(w.press('A', 100));
}

static void with_check_in_off_a_fob_is_not_held() {
  Window w(WINDOW);
  w.fob(fob(), false, false, 0);
  TEST_ASSERT_FALSE(w.waiting());
  ASSERT_SIGNAL(Signal::Unavailable, w.signal(0));
  TEST_ASSERT_FALSE(w.press('A', 100));
}

static void an_empty_uid_is_not_a_fob() {
  Window w(WINDOW);
  Uid none{};
  w.fob(none, false, true, 0);
  TEST_ASSERT_FALSE(w.waiting());
  // Nor does it cancel one that is waiting.
  w.fob(fob(), false, true, 0);
  w.fob(none, false, true, 10);
  TEST_ASSERT_TRUE(w.press('A', 20));
}

static void a_press_from_no_half_is_ignored() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 0);
  TEST_ASSERT_FALSE(w.press('C', 100));
  TEST_ASSERT_TRUE(w.waiting());
}

static void the_answer_is_shown_for_a_while_and_then_the_led_goes_dark() {
  Window w(WINDOW, 2000);
  w.fob(fob(), false, true, 0);
  w.press('A', 100);
  w.take();
  w.answered(Signal::CheckedIn, 500);
  ASSERT_SIGNAL(Signal::CheckedIn, w.signal(500));
  ASSERT_SIGNAL(Signal::CheckedIn, w.signal(2499));
  ASSERT_SIGNAL(Signal::Off, w.signal(2500));
  // And the next fob is welcome while it is still showing.
  w.fob(fob(), false, true, 600);
  TEST_ASSERT_TRUE(w.waiting());
}

/* ---------- event ids ---------- */

static void every_check_in_gets_a_higher_event_id() {
  Window w(WINDOW);
  w.seed(41);
  w.fob(fob(), false, true, 0);
  w.press('A', 10);
  TEST_ASSERT_EQUAL_UINT32(42, w.take().eventId);
  w.answered(Signal::CheckedIn, 20);
  w.fob(fob(), false, true, 30);
  w.press('B', 40);
  TEST_ASSERT_EQUAL_UINT32(43, w.take().eventId);
  TEST_ASSERT_EQUAL_UINT32(43, w.lastEventId());
}

static void a_fob_that_expires_uses_no_event_id() {
  Window w(WINDOW);
  w.seed(7);
  w.fob(fob(), false, true, 0);
  w.expire(WINDOW);
  TEST_ASSERT_EQUAL_UINT32(7, w.lastEventId());
}

/* ---------- the UID goes, and goes nowhere it should not ---------- */

static void the_window_keeps_no_uid_once_it_is_handed_over() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 0);
  w.press('A', 10);
  Request r = w.take();
  TEST_ASSERT_FALSE(r.uid.empty());
  // A second take() reads what is left behind: nothing.
  TEST_ASSERT_TRUE(holdsNoUid(w.take().uid));
}

static void clearing_a_uid_zeroes_it() {
  Uid u = fob();
  u.clear();
  TEST_ASSERT_TRUE(holdsNoUid(u));
}

static void the_request_carries_the_contract_and_the_uid() {
  Request r{42, 'A', fob()};
  const std::string body = checkin::requestBody("piezo-1", r);
  TEST_ASSERT_EQUAL_STRING(
      "{\"source\":\"piezo-1\",\"event_id\":42,\"half\":\"left\",\"uid\":\"04:A3:1B:22\"}",
      body.c_str());
  r.half = 'B';
  TEST_ASSERT_TRUE(contains(checkin::requestBody("piezo-1", r), "\"half\":\"right\""));
}

static void a_ten_byte_uid_is_written_whole() {
  Request r{1, 'A', {}};
  for (uint8_t i = 0; i < 10; i++) r.uid.bytes[i] = (uint8_t)(0xF0 + i);
  r.uid.size = 10;
  TEST_ASSERT_TRUE(contains(checkin::requestBody("s", r),
                            "\"uid\":\"F0:F1:F2:F3:F4:F5:F6:F7:F8:F9\""));
}

// The log line is built from what a check-in leaves behind, after a real one has
// gone through the window with a known card. The card number must not be in it,
// in any spelling the reader or zaehlwerk would use.
static void a_check_in_log_line_carries_no_uid() {
  Window w(WINDOW);
  w.fob(fob(), false, true, 0);
  w.press('A', 10);
  Request r = w.take();
  const std::string body = checkin::requestBody("piezo-1", r);
  TEST_ASSERT_TRUE(contains(body, "04:A3:1B:22"));  // it is there to be found

  const char *answers[] = {
      "{\"outcome\":\"claimed\",\"half\":\"left\",\"name\":\"Anna\",\"left\":\"Anna\"}",
      "", "{\"outcome\":\"unknown\",\"half\":\"left\"}"};
  const int statuses[] = {200, checkin::NO_ANSWER, 404};
  for (int i = 0; i < 3; i++) {
    const checkin::Verdict v = checkin::judge(statuses[i], answers[i]);
    const std::string line = checkin::logLine(r.half, v.outcome, r.eventId, statuses[i]);
    TEST_ASSERT_FALSE(contains(line, "04:A3:1B:22"));
    TEST_ASSERT_FALSE(contains(line, "04A31B22"));
    TEST_ASSERT_FALSE(contains(line, "04 A3 1B 22"));
    TEST_ASSERT_FALSE(contains(line, "a31b22"));
    TEST_ASSERT_TRUE(contains(line, "half A (left)"));
  }
}

static void the_log_line_says_half_and_outcome() {
  TEST_ASSERT_EQUAL_STRING("[checkin] half B (right) claimed event_id 43 http 200",
                           checkin::logLine('B', "claimed", 43, 200).c_str());
  TEST_ASSERT_EQUAL_STRING("[checkin] expired",
                           checkin::logLine(' ', "expired", 0, 0).c_str());
}

/* ---------- reading the answer ---------- */

static void every_claiming_outcome_reads_as_checked_in() {
  const char *ok[] = {"claimed", "unchanged", "replaced", "moved", "started"};
  for (const char *o : ok) {
    const std::string body = std::string("{\"outcome\":\"") + o + "\",\"half\":\"left\"}";
    const checkin::Verdict v = checkin::judge(200, body);
    TEST_ASSERT_EQUAL_STRING(o, v.outcome);
    ASSERT_SIGNAL(Signal::CheckedIn, v.signal);
  }
}

static void a_duplicate_is_not_a_check_in() {
  const checkin::Verdict v = checkin::judge(200, "{\"outcome\":\"duplicate\",\"half\":\"left\"}");
  TEST_ASSERT_EQUAL_STRING("duplicate", v.outcome);
  ASSERT_SIGNAL(Signal::Rejected, v.signal);
}

static void a_repeated_answer_reads_like_the_first() {
  const checkin::Verdict v =
      checkin::judge(200, "{\"outcome\":\"claimed\",\"half\":\"left\",\"repeated\":true}");
  ASSERT_SIGNAL(Signal::CheckedIn, v.signal);
}

static void the_statuses_map_to_the_led() {
  ASSERT_SIGNAL(Signal::Unknown, checkin::judge(404, "").signal);
  ASSERT_SIGNAL(Signal::Rejected, checkin::judge(409, "{\"outcome\":\"refused\"}").signal);
  ASSERT_SIGNAL(Signal::Unavailable, checkin::judge(503, "").signal);
  ASSERT_SIGNAL(Signal::Unavailable, checkin::judge(500, "").signal);
  ASSERT_SIGNAL(Signal::Unavailable, checkin::judge(400, "").signal);
  ASSERT_SIGNAL(Signal::Unavailable, checkin::judge(502, "").signal);
  ASSERT_SIGNAL(Signal::Unavailable, checkin::judge(checkin::NO_ANSWER, "").signal);
  ASSERT_SIGNAL(Signal::Unavailable, checkin::judge(checkin::NO_WIFI, "").signal);
  TEST_ASSERT_EQUAL_STRING("no_wifi", checkin::judge(checkin::NO_WIFI, "").outcome);
  TEST_ASSERT_EQUAL_STRING("bad_request", checkin::judge(400, "").outcome);
}

static void an_outcome_not_on_the_list_is_never_copied_into_the_log() {
  const checkin::Verdict v = checkin::judge(200, "{\"outcome\":\"04:A3:1B:22\"}");
  TEST_ASSERT_EQUAL_STRING("ok", v.outcome);
}

static void only_no_answer_at_all_is_retried() {
  TEST_ASSERT_TRUE(checkin::worthRetrying(checkin::NO_ANSWER));
  TEST_ASSERT_FALSE(checkin::worthRetrying(checkin::NO_WIFI));
  TEST_ASSERT_FALSE(checkin::worthRetrying(503));
  TEST_ASSERT_FALSE(checkin::worthRetrying(200));
}

/* ---------- the LED ---------- */

static void waiting_blinks_green_and_the_answers_are_told_apart() {
  TEST_ASSERT_TRUE(checkin::lights(Signal::Waiting, 0).green);
  TEST_ASSERT_FALSE(checkin::lights(Signal::Waiting, 300).green);
  TEST_ASSERT_FALSE(checkin::lights(Signal::Waiting, 0).red);

  const Lights in = checkin::lights(Signal::CheckedIn, 1000);
  TEST_ASSERT_TRUE(in.green && !in.red);
  const Lights unknown = checkin::lights(Signal::Unknown, 1000);
  TEST_ASSERT_TRUE(!unknown.green && unknown.red);
  // Rejected blinks slower than unavailable, so the two are told apart.
  TEST_ASSERT_TRUE(checkin::lights(Signal::Rejected, 150).red);
  TEST_ASSERT_FALSE(checkin::lights(Signal::Unavailable, 150).red);
  // Expired blinks twice and goes dark.
  TEST_ASSERT_TRUE(checkin::lights(Signal::Expired, 0).red);
  TEST_ASSERT_FALSE(checkin::lights(Signal::Expired, 200).red);
  TEST_ASSERT_TRUE(checkin::lights(Signal::Expired, 350).red);
  TEST_ASSERT_FALSE(checkin::lights(Signal::Expired, 700).red);
  const Lights off = checkin::lights(Signal::Off, 0);
  TEST_ASSERT_FALSE(off.green || off.red);
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(a_fob_then_a_press_is_a_check_in_at_that_half);
  RUN_TEST(a_press_with_no_fob_is_not_a_check_in);
  RUN_TEST(only_the_first_press_is_taken);
  RUN_TEST(no_press_within_the_window_drops_the_fob);
  RUN_TEST(a_late_press_is_not_a_check_in_even_before_the_expiry_is_noticed);
  RUN_TEST(the_window_survives_the_millis_wrap);
  RUN_TEST(a_second_fob_replaces_the_first_and_starts_the_window_again);
  RUN_TEST(a_fob_while_a_request_is_out_changes_nothing);
  RUN_TEST(a_fob_during_play_does_nothing_and_the_press_stays_a_point);
  RUN_TEST(with_check_in_off_a_fob_is_not_held);
  RUN_TEST(an_empty_uid_is_not_a_fob);
  RUN_TEST(a_press_from_no_half_is_ignored);
  RUN_TEST(the_answer_is_shown_for_a_while_and_then_the_led_goes_dark);

  RUN_TEST(every_check_in_gets_a_higher_event_id);
  RUN_TEST(a_fob_that_expires_uses_no_event_id);

  RUN_TEST(the_window_keeps_no_uid_once_it_is_handed_over);
  RUN_TEST(clearing_a_uid_zeroes_it);
  RUN_TEST(the_request_carries_the_contract_and_the_uid);
  RUN_TEST(a_ten_byte_uid_is_written_whole);
  RUN_TEST(a_check_in_log_line_carries_no_uid);
  RUN_TEST(the_log_line_says_half_and_outcome);

  RUN_TEST(every_claiming_outcome_reads_as_checked_in);
  RUN_TEST(a_duplicate_is_not_a_check_in);
  RUN_TEST(a_repeated_answer_reads_like_the_first);
  RUN_TEST(the_statuses_map_to_the_led);
  RUN_TEST(an_outcome_not_on_the_list_is_never_copied_into_the_log);
  RUN_TEST(only_no_answer_at_all_is_retried);

  RUN_TEST(waiting_blinks_green_and_the_answers_are_told_apart);

  return UNITY_END();
}
