// Tests of reporting to zaehlwerk-api: the address, the answers, the queue, the
// retries and the join (#17, #41, #66, #67, #69, ADR-0008).
//
// Everything here runs against lib/report alone. The HTTP client, NVS and the
// task are the firmware's; what they are asked to do, and what they make of
// the answer, is decided here.

#include <unity.h>

#include <string.h>

#include <string>

#include "report.h"

using report::Admit;
using report::Kind;
using report::Link;
using report::Next;
using report::Phase;
using report::Record;
using report::Request;
using report::State;
using report::UrlProblem;

void setUp() {}
void tearDown() {}

namespace {

// An answer the way zaehlwerk writes it: scorer.State, ends included, with a
// field the board does not read (completed_sets) in the middle.
std::string stateJson(const char *id, int pa, int pb, const char *serving = "a",
                      const char *phase = "playing", const char *ends = "\"a\",\"b\"",
                      bool complete = false, const char *winner = "") {
  std::string j = std::string("{\"match_id\":\"") + id +
                  "\",\"players\":[\"Pat\",\"Ana\"],\"points\":[" + std::to_string(pa) + "," +
                  std::to_string(pb) + "],\"sets\":[0,0],\"set_number\":1,\"serving\":\"" +
                  serving + "\",\"complete\":" + (complete ? "true" : "false");
  if (winner[0]) j += std::string(",\"winner\":\"") + winner + "\"";
  j += std::string(",\"completed_sets\":[[11,9],[7,11]],\"phase\":\"") + phase +
       "\",\"ends\":[" + ends + "]}";
  return j;
}

const char *SRC = "piezo-1";

// A link that has joined match m-1 at 0:0 and has nothing queued or recorded.
void joined(Link &l, uint32_t now = 0) {
  l.configure(true, now);
  Request r = l.next(now, 0);
  l.answered(r, 200, stateJson("m-1", 0, 0), now, 0);
  State s;
  l.takeAdoption(s);
  Record rec;
  while (l.takeRecord(rec)) {
  }
}

// Queues a point the way the firmware does: admit, then an id, then push.
uint32_t nextId = 100;
bool point(Link &l, char half, uint8_t delta = 1, bool manual = false, uint32_t now = 0) {
  const Admit a = l.admit(Kind::Point, manual);
  if (a != Admit::Queue) {
    l.refused(a, Kind::Point, half, delta);
    return false;
  }
  return l.push(Kind::Point, half, delta, ++nextId, now);
}

int countRecords(Link &l, const char *status) {
  int n = 0;
  Record r;
  while (l.takeRecord(r))
    if (std::string(r.status) == status) n++;
  return n;
}

}  // namespace

/* ---------- the address (#66) ---------- */

static void an_http_address_with_a_port_is_usable() {
  TEST_ASSERT_EQUAL(UrlProblem::None, report::checkUrl("http://10.0.0.5:8080"));
  TEST_ASSERT_EQUAL(UrlProblem::None, report::checkUrl("http://zaehlwerk.lab.example"));
  TEST_ASSERT_EQUAL(UrlProblem::None, report::checkUrl("http://10.0.0.5:8080/"));
}

static void a_trailing_slash_is_taken_off() {
  TEST_ASSERT_EQUAL_STRING("http://10.0.0.5:8080", report::normaliseUrl("http://10.0.0.5:8080/").c_str());
  TEST_ASSERT_EQUAL_STRING("http://10.0.0.5", report::normaliseUrl("http://10.0.0.5").c_str());
}

static void https_is_refused_with_a_reason_rather_than_failing_later() {
  TEST_ASSERT_EQUAL(UrlProblem::Https, report::checkUrl("https://zaehlwerk.example"));
  TEST_ASSERT_TRUE(std::string(report::urlProblemText(UrlProblem::Https)).find("https") !=
                   std::string::npos);
}

static void a_local_name_is_refused_because_the_client_does_not_ask_mdns() {
  TEST_ASSERT_EQUAL(UrlProblem::Local, report::checkUrl("http://zaehlwerk.local:8080"));
  TEST_ASSERT_EQUAL(UrlProblem::Local, report::checkUrl("http://Laptop.LOCAL"));
}

static void an_address_with_a_path_is_refused() {
  TEST_ASSERT_EQUAL(UrlProblem::Path, report::checkUrl("http://10.0.0.5:8080/ingest/piezo"));
  TEST_ASSERT_EQUAL(UrlProblem::Path, report::checkUrl("http://10.0.0.5//"));
}

static void spaces_umlauts_and_control_characters_are_refused() {
  TEST_ASSERT_EQUAL(UrlProblem::Chars, report::checkUrl("http://10.0.0.5 "));
  TEST_ASSERT_EQUAL(UrlProblem::Chars, report::checkUrl("http://z\xc3\xa4hlwerk"));
  TEST_ASSERT_EQUAL(UrlProblem::Chars, report::checkUrl("http://10.0.0.5\r\n"));
}

static void a_port_that_is_not_one_is_refused() {
  TEST_ASSERT_EQUAL(UrlProblem::Port, report::checkUrl("http://10.0.0.5:"));
  TEST_ASSERT_EQUAL(UrlProblem::Port, report::checkUrl("http://10.0.0.5:0"));
  TEST_ASSERT_EQUAL(UrlProblem::Port, report::checkUrl("http://10.0.0.5:65536"));
  TEST_ASSERT_EQUAL(UrlProblem::Port, report::checkUrl("http://10.0.0.5:80x"));
}

static void anything_but_a_plain_host_is_refused() {
  TEST_ASSERT_EQUAL(UrlProblem::Scheme, report::checkUrl("10.0.0.5:8080"));
  TEST_ASSERT_EQUAL(UrlProblem::Scheme, report::checkUrl("ftp://10.0.0.5"));
  TEST_ASSERT_EQUAL(UrlProblem::Host, report::checkUrl("http://"));
  TEST_ASSERT_EQUAL(UrlProblem::Host, report::checkUrl("http://user@10.0.0.5"));
  TEST_ASSERT_EQUAL(UrlProblem::Host, report::checkUrl("http://10.0.0.5?x=1"));
  TEST_ASSERT_EQUAL(UrlProblem::Length, report::checkUrl(std::string("http://") + std::string(100, 'a')));
}

/* ---------- reading zaehlwerk's answers ---------- */

static void the_state_of_an_ingest_answer_is_read() {
  State s;
  TEST_ASSERT_TRUE(report::parseState(stateJson("m-7f2a91", 3, 2, "b", "playing", "\"b\",\"a\""), s));
  TEST_ASSERT_TRUE(s.valid);
  TEST_ASSERT_EQUAL_STRING("m-7f2a91", s.matchId.c_str());
  TEST_ASSERT_EQUAL_STRING("Pat", s.players[0].c_str());
  TEST_ASSERT_EQUAL(3, s.points[0]);
  TEST_ASSERT_EQUAL(2, s.points[1]);
  TEST_ASSERT_EQUAL_CHAR('b', s.serving);
  TEST_ASSERT_EQUAL_CHAR('b', s.ends[0]);
  TEST_ASSERT_EQUAL_STRING("playing", s.phase.c_str());
  TEST_ASSERT_FALSE(s.demo);
}

static void the_state_inside_an_error_body_is_read() {
  // A 409 for a won match carries the state as it stands.
  State s;
  const std::string b = "{\"error\":\"the match is over\",\"state\":" +
                        stateJson("m-1", 11, 9, "a", "complete", "\"a\",\"b\"", true, "a") + "}";
  TEST_ASSERT_TRUE(report::parseState(b, s));
  TEST_ASSERT_TRUE(s.complete);
  TEST_ASSERT_EQUAL_CHAR('a', s.winner);
}

static void an_error_without_a_state_is_no_state() {
  State s;
  TEST_ASSERT_FALSE(report::parseState("{\"error\":\"no match is running\"}", s));
  TEST_ASSERT_FALSE(report::parseState("{\"error\":\"x\",\"state\":null}", s));
  TEST_ASSERT_FALSE(report::parseState("<html>503 Service Unavailable</html>", s));
  TEST_ASSERT_FALSE(report::parseState("", s));
}

static void a_demo_is_recognised_by_its_flag() {
  State s;
  std::string b = stateJson("m-2", 0, 0);
  b.insert(b.size() - 1, ",\"demo\":true");
  TEST_ASSERT_TRUE(report::parseState(b, s));
  TEST_ASSERT_TRUE(s.demo);
}

static void names_with_escapes_and_umlauts_come_through_as_utf8() {
  State s;
  std::string b = stateJson("m-1", 0, 0);
  const std::string from = "[\"Pat\",\"Ana\"]";
  b.replace(b.find(from), from.size(), "[\"J\\u00fcrgen \\\"J\\\"\",\"Zo\xc3\xab\"]");
  TEST_ASSERT_TRUE(report::parseState(b, s));
  TEST_ASSERT_EQUAL_STRING("J\xc3\xbcrgen \"J\"", s.players[0].c_str());
  TEST_ASSERT_EQUAL_STRING("Zo\xc3\xab", s.players[1].c_str());
}

static void a_half_read_answer_is_not_taken_for_a_state() {
  State s;
  const std::string b = stateJson("m-1", 3, 2);
  TEST_ASSERT_FALSE(report::parseState(b.substr(0, b.size() - 10), s));
  TEST_ASSERT_FALSE(s.valid);
}

static void an_answer_that_does_not_hold_together_is_refused() {
  State s;
  TEST_ASSERT_FALSE(report::parseState(stateJson("m-1\\\"x", 0, 0), s));       // odd match id
  TEST_ASSERT_FALSE(report::parseState(stateJson("m-1", 0, 0, "c"), s));       // a third player
  TEST_ASSERT_FALSE(report::parseState(stateJson("m-1", 0, 0, "a", "playing", "\"a\",\"a\""), s));
  TEST_ASSERT_FALSE(report::parseState(stateJson("m-1", -1, 0), s));
}

/* ---------- what the board shows ---------- */

static void after_a_change_of_ends_the_points_follow_the_players() {
  // Player b at the left half now: b's points belong to half A.
  State s;
  report::parseState(stateJson("m-1", 3, 7, "a", "playing", "\"b\",\"a\""), s);
  report::Board b = report::boardFrom(s, 'A');
  TEST_ASSERT_EQUAL(7, b.pointsA);
  TEST_ASSERT_EQUAL(3, b.pointsB);
  TEST_ASSERT_EQUAL(1, b.sideToPlayer[0]);
  TEST_ASSERT_EQUAL(0, b.sideToPlayer[1]);
  TEST_ASSERT_EQUAL_CHAR('B', b.serve);  // a serves, and a is at the right
}

static void the_serve_comes_from_zaehlwerk_and_the_rotation_is_seeded_to_match() {
  // After an undo on the phone the serve is whatever zaehlwerk says, and the
  // board's own rotation has to arrive at the same half for the next point.
  State s;
  report::parseState(stateJson("m-1", 1, 0, "b"), s);
  report::Board b = report::boardFrom(s, 'A');
  TEST_ASSERT_EQUAL_CHAR('B', b.serve);
  TEST_ASSERT_EQUAL_CHAR('B', b.firstServe);  // 1:0 is still the first server's turn
  report::parseState(stateJson("m-1", 2, 1, "b"), s);
  b = report::boardFrom(s, 'A');
  TEST_ASSERT_EQUAL_CHAR('A', b.firstServe);  // three points in, the second server
}

static void during_the_serve_rally_the_board_keeps_its_own_serve() {
  State s;
  report::parseState(stateJson("m-1", 0, 0, "", "serve_rally"), s);
  TEST_ASSERT_EQUAL_CHAR('B', report::boardFrom(s, 'B').serve);
}

static void the_winner_is_named_by_half() {
  State s;
  report::parseState(stateJson("m-1", 9, 11, "a", "complete", "\"b\",\"a\"", true, "b"), s);
  report::Board b = report::boardFrom(s, 'A');
  TEST_ASSERT_TRUE(b.over);
  TEST_ASSERT_EQUAL_CHAR('A', b.winner);
}

static void an_awarded_rally_is_a_point_for_that_half() {
  report::Verdict v = report::fromRally('B', "ABAB");
  TEST_ASSERT_EQUAL_CHAR('B', v.half);
  TEST_ASSERT_EQUAL(1, v.delta);
}

static void a_rally_awarded_to_nobody_is_sent_as_delta_zero() {
  report::Verdict v = report::fromRally(' ', "B");
  TEST_ASSERT_EQUAL_CHAR('B', v.half);
  TEST_ASSERT_EQUAL(0, v.delta);
  TEST_ASSERT_EQUAL_CHAR(0, report::fromRally(' ', "").half);
}

/* ---------- the event on the wire ---------- */

static void a_point_names_its_half_never_its_player() {
  report::Event e;
  e.eventId = 42;
  e.kind = Kind::Point;
  e.half = 'A';
  e.delta = 1;
  strcpy(e.matchId, "m-7f2a91");
  TEST_ASSERT_EQUAL_STRING(
      "{\"match_id\":\"m-7f2a91\",\"source\":\"piezo-1\",\"half\":\"left\",\"delta\":1,\"event_id\":42}",
      report::body(SRC, e).c_str());
  e.half = 'B';
  e.delta = 0;
  TEST_ASSERT_TRUE(report::body(SRC, e).find("\"half\":\"right\",\"delta\":0") != std::string::npos);
}

static void an_undo_carries_no_half_and_no_delta() {
  report::Event e;
  e.eventId = 43;
  e.kind = Kind::Undo;
  e.half = 'A';
  e.delta = 1;
  strcpy(e.matchId, "m-1");
  TEST_ASSERT_EQUAL_STRING(
      "{\"match_id\":\"m-1\",\"source\":\"piezo-1\",\"event_id\":43,\"action\":\"undo\"}",
      report::body(SRC, e).c_str());
}

static void a_source_with_a_quote_cannot_break_the_body() {
  report::Event e;
  strcpy(e.matchId, "m-1");
  e.half = 'A';
  TEST_ASSERT_TRUE(report::body("pie\"zo", e).find("\"source\":\"pie\\\"zo\"") != std::string::npos);
}

/* ---------- sending again (#69) ---------- */

static void no_answer_and_gateway_errors_are_sent_again() {
  TEST_ASSERT_EQUAL(Next::Retry, report::classify(report::NO_ANSWER));
  TEST_ASSERT_EQUAL(Next::Retry, report::classify(report::NO_WIFI));
  TEST_ASSERT_EQUAL(Next::Retry, report::classify(502));
  TEST_ASSERT_EQUAL(Next::Retry, report::classify(503));
  TEST_ASSERT_EQUAL(Next::Retry, report::classify(504));
}

// Like the zaehlwerk piezo mock, the reference client: a 5xx is never the
// last word on an event, whoever said it.
static void every_5xx_is_sent_again_like_the_reference_mock() {
  TEST_ASSERT_EQUAL(Next::Retry, report::classify(500));
  TEST_ASSERT_EQUAL(Next::Retry, report::classify(501));
  TEST_ASSERT_EQUAL(Next::Retry, report::classify(599));
  TEST_ASSERT_EQUAL(Next::Drop, report::classify(600));
}

static void a_500_holds_the_queue_and_keeps_the_event_id() {
  Link l;
  joined(l);
  point(l, 'A');
  point(l, 'B');
  Request first = l.next(0, 0);
  l.answered(first, 500, "{\"error\":\"x\"}", 0, 0);
  TEST_ASSERT_EQUAL(2, (int)l.queued());
  TEST_ASSERT_EQUAL_UINT32(0, l.status(0).dropped);
  Request again = l.next(1500, 0);
  TEST_ASSERT_EQUAL_UINT32(first.event.eventId, again.event.eventId);
}

static void any_other_answer_is_final() {
  TEST_ASSERT_EQUAL(Next::Done, report::classify(200));
  TEST_ASSERT_EQUAL(Next::Drop, report::classify(400));
  TEST_ASSERT_EQUAL(Next::Drop, report::classify(422));
  TEST_ASSERT_EQUAL(Next::Drop, report::classify(301));
  TEST_ASSERT_EQUAL(Next::MatchGone, report::classify(404));
  TEST_ASSERT_EQUAL(Next::MatchGone, report::classify(409));
}

static void the_wait_doubles_from_one_second_up_to_thirty() {
  TEST_ASSERT_EQUAL_UINT32(1000, report::backoffMs(1, 0));
  TEST_ASSERT_EQUAL_UINT32(2000, report::backoffMs(2, 0));
  TEST_ASSERT_EQUAL_UINT32(4000, report::backoffMs(3, 0));
  TEST_ASSERT_EQUAL_UINT32(30000, report::backoffMs(6, 0));
  TEST_ASSERT_EQUAL_UINT32(30000, report::backoffMs(1000, 0));
}

static void the_jitter_adds_at_most_half_again() {
  TEST_ASSERT_EQUAL_UINT32(1500, report::backoffMs(1, 500));
  TEST_ASSERT_EQUAL_UINT32(1000, report::backoffMs(1, 501));  // wraps, never above
  TEST_ASSERT_TRUE(report::backoffMs(9, 0xffffffffu) <= 45000);
}

/* ---------- the queue (#17) ---------- */

static void a_full_queue_refuses_the_new_event_and_keeps_the_old() {
  report::Queue q;
  report::Event e;
  for (uint32_t i = 0; i < report::Queue::CAPACITY; i++) {
    e.eventId = i + 1;
    TEST_ASSERT_TRUE(q.push(e));
  }
  e.eventId = 999;
  TEST_ASSERT_FALSE(q.push(e));
  TEST_ASSERT_EQUAL_UINT32(1, q.front().eventId);
}

static void the_queue_keeps_its_order_across_the_wrap() {
  report::Queue q;
  report::Event e;
  for (uint32_t i = 1; i <= 200; i++) {
    e.eventId = i;
    q.push(e);
    if (q.size() > 3) {
      TEST_ASSERT_EQUAL_UINT32(i - 3, q.front().eventId);
      q.pop();
    }
  }
}

/* ---------- the link: opt-in ---------- */

// Hard requirement: no address, no difference. Nothing is asked, nothing is
// queued, nothing is logged and no score is handed to the board.
static void without_an_address_the_link_asks_for_nothing_and_says_nothing() {
  Link l;
  TEST_ASSERT_FALSE(l.configured());
  for (uint32_t t = 0; t < 600000; t += 997) {
    TEST_ASSERT_EQUAL(Request::Type::None, l.next(t, t).type);
  }
  TEST_ASSERT_EQUAL(Admit::NotConfigured, l.admit(Kind::Point, false));
  TEST_ASSERT_EQUAL(Admit::NotConfigured, l.admit(Kind::Point, true));
  TEST_ASSERT_EQUAL(Admit::NotConfigured, l.admit(Kind::Undo, true));
  l.refused(Admit::NotConfigured, Kind::Point, 'A', 1);
  TEST_ASSERT_FALSE(l.push(Kind::Point, 'A', 1, 1, 0));
  Request fake;
  fake.type = Request::Type::Join;
  l.answered(fake, 200, stateJson("m-1", 5, 5), 10, 0);
  State s;
  Record r;
  TEST_ASSERT_FALSE(l.takeAdoption(s));
  TEST_ASSERT_FALSE(l.takeRecord(r));
  TEST_ASSERT_EQUAL(0, (int)l.queued());
  TEST_ASSERT_EQUAL(Phase::Off, l.phase());
  TEST_ASSERT_TRUE(report::statusJson(l.status(0), "", SRC, 0, false).find("\"configured\":false") !=
                   std::string::npos);
}

static void clearing_the_address_makes_it_quiet_again() {
  Link l;
  joined(l);
  l.configure(false, 50);
  TEST_ASSERT_EQUAL(Request::Type::None, l.next(100000, 0).type);
  TEST_ASSERT_EQUAL(Admit::NotConfigured, l.admit(Kind::Point, true));
}

/* ---------- the link: joining (#41) ---------- */

static void a_configured_link_first_asks_for_the_running_match() {
  Link l;
  l.configure(true, 0);
  TEST_ASSERT_EQUAL(Phase::Waiting, l.phase());
  TEST_ASSERT_EQUAL(Request::Type::Join, l.next(0, 0).type);
  TEST_ASSERT_EQUAL(Admit::NotJoined, l.admit(Kind::Point, true));
}

static void with_no_match_running_the_board_waits_and_asks_again_after_two_seconds() {
  Link l;
  l.configure(true, 0);
  l.answered(l.next(0, 0), 404, "{\"error\":\"no match is running\"}", 0, 0);
  TEST_ASSERT_EQUAL(Phase::Waiting, l.phase());
  TEST_ASSERT_EQUAL(Request::Type::None, l.next(1999, 0).type);
  TEST_ASSERT_EQUAL(Request::Type::Join, l.next(2000, 0).type);
}

static void a_demo_is_not_joined_and_nothing_is_sent_into_it() {
  Link l;
  l.configure(true, 0);
  std::string b = stateJson("m-demo", 4, 2);
  b.insert(b.size() - 1, ",\"demo\":true");
  l.answered(l.next(0, 0), 200, b, 0, 0);
  TEST_ASSERT_EQUAL(Phase::Demo, l.phase());
  TEST_ASSERT_EQUAL(Admit::NotJoined, l.admit(Kind::Point, false));
  State s;
  TEST_ASSERT_FALSE(l.takeAdoption(s));
  TEST_ASSERT_EQUAL(1, countRecords(l, "demo"));
}

static void a_running_match_is_joined_and_its_score_handed_to_the_board() {
  Link l;
  l.configure(true, 0);
  l.answered(l.next(0, 0), 200, stateJson("m-1", 4, 2), 0, 0);
  TEST_ASSERT_EQUAL(Phase::Joined, l.phase());
  State s;
  TEST_ASSERT_TRUE(l.takeAdoption(s));
  TEST_ASSERT_EQUAL(4, s.points[0]);
  TEST_ASSERT_FALSE(l.takeAdoption(s));  // handed over once
  TEST_ASSERT_EQUAL(1, countRecords(l, "joined"));
}

static void an_undo_on_the_phone_reaches_the_board_with_the_next_poll() {
  Link l;
  joined(l);
  Request r = l.next(Link::POLL_MS, 0);
  TEST_ASSERT_EQUAL(Request::Type::Join, r.type);
  l.answered(r, 200, stateJson("m-1", 0, 0, "b"), Link::POLL_MS, 0);
  State s;
  TEST_ASSERT_TRUE(l.takeAdoption(s));
  TEST_ASSERT_EQUAL_CHAR('b', s.serving);
}

static void a_new_match_on_the_page_is_joined_in_place_of_the_old_one() {
  Link l;
  joined(l);
  l.answered(l.next(Link::POLL_MS, 0), 200, stateJson("m-2", 0, 0), Link::POLL_MS, 0);
  TEST_ASSERT_EQUAL_STRING("m-2", l.match().matchId.c_str());
  TEST_ASSERT_EQUAL(1, countRecords(l, "joined"));
}

static void a_match_ended_on_the_page_sends_the_board_back_to_waiting() {
  Link l;
  joined(l);
  l.answered(l.next(Link::POLL_MS, 0), 404, "{\"error\":\"no match\"}", Link::POLL_MS, 0);
  TEST_ASSERT_EQUAL(Phase::Waiting, l.phase());
  TEST_ASSERT_EQUAL(1, countRecords(l, "left"));
}

static void a_failed_poll_during_a_rollout_keeps_the_match() {
  Link l;
  joined(l);
  l.answered(l.next(Link::POLL_MS, 0), 503, "<html>", Link::POLL_MS, 0);
  TEST_ASSERT_EQUAL(Phase::Joined, l.phase());
  TEST_ASSERT_EQUAL(Admit::Queue, l.admit(Kind::Point, false));
  TEST_ASSERT_FALSE(l.status(Link::POLL_MS).reachable);
  TEST_ASSERT_EQUAL(1, countRecords(l, "unreachable"));
}

/* ---------- the link: sending (#17, #69) ---------- */

static void a_point_is_sent_with_the_match_and_its_own_event_id() {
  Link l;
  joined(l);
  TEST_ASSERT_TRUE(point(l, 'B'));
  Request r = l.next(0, 0);
  TEST_ASSERT_EQUAL(Request::Type::Send, r.type);
  TEST_ASSERT_EQUAL_UINT32(nextId, r.event.eventId);
  TEST_ASSERT_EQUAL_STRING("m-1", r.event.matchId);
  TEST_ASSERT_EQUAL_CHAR('B', r.event.half);
  l.answered(r, 200, stateJson("m-1", 0, 1), 10, 0);
  TEST_ASSERT_EQUAL(0, (int)l.queued());
  State s;
  TEST_ASSERT_TRUE(l.takeAdoption(s));
  TEST_ASSERT_EQUAL(1, s.points[1]);
}

static void a_lost_answer_sends_the_same_event_id_again() {
  Link l;
  joined(l);
  point(l, 'A');
  Request first = l.next(0, 0);
  l.answered(first, report::NO_ANSWER, "", 10, 0);
  TEST_ASSERT_EQUAL(1, (int)l.queued());
  TEST_ASSERT_EQUAL(Request::Type::None, l.next(500, 0).type);  // backing off
  Request again = l.next(1010, 0);
  TEST_ASSERT_EQUAL(Request::Type::Send, again.type);
  TEST_ASSERT_EQUAL_UINT32(first.event.eventId, again.event.eventId);
  TEST_ASSERT_EQUAL(1, again.event.attempts);
}

static void a_rollout_is_ridden_out_with_growing_waits_and_the_same_id() {
  Link l;
  joined(l);
  point(l, 'A');
  const uint32_t id = l.next(0, 0).event.eventId;
  uint32_t now = 0;
  const uint32_t waits[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
  for (uint32_t w : waits) {
    Request r = l.next(now, 0);
    TEST_ASSERT_EQUAL(Request::Type::Send, r.type);
    TEST_ASSERT_EQUAL_UINT32(id, r.event.eventId);
    l.answered(r, 503, "upstream connect error", now, 0);
    TEST_ASSERT_EQUAL(Request::Type::None, l.next(now + w - 1, 0).type);
    now += w;
  }
  l.answered(l.next(now, 0), 200, stateJson("m-1", 1, 0), now, 0);
  TEST_ASSERT_EQUAL(0, (int)l.queued());
  TEST_ASSERT_TRUE(l.status(now).reachable);
  Record r;
  int back = 0, retries = 0;
  while (l.takeRecord(r)) {
    if (std::string(r.status) == "back") back++;
    if (std::string(r.status) == "retry") retries++;
  }
  TEST_ASSERT_EQUAL(1, back);
  TEST_ASSERT_EQUAL(7, retries);
}

static void queued_events_leave_in_order_once_zaehlwerk_is_back() {
  Link l;
  joined(l);
  point(l, 'A');
  l.answered(l.next(0, 0), report::NO_WIFI, "", 0, 0);
  point(l, 'B');
  point(l, 'A', 0);
  TEST_ASSERT_TRUE(l.admit(Kind::Undo, false) == Admit::Queue);
  l.push(Kind::Undo, 0, 0, ++nextId, 0);
  uint32_t last = 0;
  uint32_t now = 1000;
  int sent = 0;
  while (l.queued()) {
    Request r = l.next(now, 0);
    TEST_ASSERT_EQUAL(Request::Type::Send, r.type);
    TEST_ASSERT_TRUE(r.event.eventId > last);
    last = r.event.eventId;
    l.answered(r, 200, stateJson("m-1", 1, 0), now, 0);
    sent++;
  }
  TEST_ASSERT_EQUAL(4, sent);
}

static void an_older_answer_does_not_take_back_a_point_queued_after_it() {
  Link l;
  joined(l);
  // The poll goes out at 0:0 ...
  Request poll = l.next(Link::POLL_MS, 0);
  TEST_ASSERT_EQUAL(Request::Type::Join, poll.type);
  // ... a rally ends on the board while it is in flight ...
  point(l, 'A');
  // ... and the answer still says 0:0. Showing it would take the point away.
  l.answered(poll, 200, stateJson("m-1", 0, 0), Link::POLL_MS, 0);
  State s;
  TEST_ASSERT_FALSE(l.takeAdoption(s));
  // The answer to the point itself is the one to show.
  l.answered(l.next(Link::POLL_MS, 0), 200, stateJson("m-1", 1, 0), Link::POLL_MS, 0);
  TEST_ASSERT_TRUE(l.takeAdoption(s));
  TEST_ASSERT_EQUAL(1, s.points[0]);
}

static void an_answer_is_not_shown_while_later_points_still_wait() {
  Link l;
  joined(l);
  point(l, 'A');
  point(l, 'B');
  l.answered(l.next(0, 0), 200, stateJson("m-1", 1, 0), 0, 0);
  State s;
  TEST_ASSERT_FALSE(l.takeAdoption(s));  // the board already shows 1:1
  l.answered(l.next(0, 0), 200, stateJson("m-1", 1, 1), 0, 0);
  TEST_ASSERT_TRUE(l.takeAdoption(s));
}

static void a_refused_event_is_dropped_and_the_next_one_goes() {
  Link l;
  joined(l);
  point(l, 'A');
  point(l, 'B');
  l.answered(l.next(0, 0), 400, "{\"error\":\"event 0: bad\"}", 0, 0);
  TEST_ASSERT_EQUAL(1, (int)l.queued());
  Request r = l.next(0, 0);
  TEST_ASSERT_EQUAL(Request::Type::Send, r.type);
  TEST_ASSERT_EQUAL_CHAR('B', r.event.half);
  TEST_ASSERT_EQUAL_UINT32(1, l.status(0).dropped);
}

static void a_match_over_on_the_page_drops_its_events_and_the_board_waits() {
  Link l;
  joined(l);
  point(l, 'A');
  point(l, 'B');
  l.answered(l.next(0, 0), 404, "{\"error\":\"unknown match\"}", 0, 0);
  TEST_ASSERT_EQUAL(0, (int)l.queued());
  TEST_ASSERT_EQUAL(Phase::Waiting, l.phase());
  TEST_ASSERT_EQUAL(2, countRecords(l, "dropped"));
}

static void a_detected_rally_during_warmup_is_not_sent_but_a_point_given_by_hand_is() {
  Link l;
  l.configure(true, 0);
  l.answered(l.next(0, 0), 200, stateJson("m-1", 0, 0, "", "warmup"), 0, 0);
  TEST_ASSERT_EQUAL(Admit::SkippedPhase, l.admit(Kind::Point, false));
  TEST_ASSERT_EQUAL(Admit::Queue, l.admit(Kind::Point, true));
  TEST_ASSERT_EQUAL(Admit::Queue, l.admit(Kind::Undo, false));
}

static void a_detected_rally_decides_the_serve_rally() {
  Link l;
  l.configure(true, 0);
  l.answered(l.next(0, 0), 200, stateJson("m-1", 0, 0, "", "serve_rally"), 0, 0);
  TEST_ASSERT_EQUAL(Admit::Queue, l.admit(Kind::Point, false));
}

static void after_a_won_match_an_undo_still_goes_but_a_point_does_not() {
  Link l;
  joined(l);
  point(l, 'A');
  l.answered(l.next(0, 0), 200, stateJson("m-1", 11, 9, "a", "complete", "\"a\",\"b\"", true, "a"), 0, 0);
  TEST_ASSERT_EQUAL(Phase::Finished, l.phase());
  // The won match is no longer running, so /matches/current has nothing.
  l.answered(l.next(Link::POLL_MS, 0), 404, "{}", Link::POLL_MS, 0);
  TEST_ASSERT_EQUAL(Phase::Finished, l.phase());
  TEST_ASSERT_EQUAL(Admit::MatchOver, l.admit(Kind::Point, true));
  TEST_ASSERT_EQUAL(Admit::Queue, l.admit(Kind::Undo, false));
  l.push(Kind::Undo, 0, 0, ++nextId, 0);
  Request r = l.next(Link::POLL_MS, 0);
  TEST_ASSERT_EQUAL_STRING("m-1", r.event.matchId);  // named, or it would not reach a won match
  l.answered(r, 200, stateJson("m-1", 10, 9), Link::POLL_MS, 0);
  TEST_ASSERT_EQUAL(Phase::Joined, l.phase());
}

static void a_full_queue_is_said_in_the_log() {
  Link l;
  joined(l);
  for (size_t i = 0; i < report::Queue::CAPACITY; i++) TEST_ASSERT_TRUE(point(l, 'A'));
  TEST_ASSERT_FALSE(point(l, 'B'));
  TEST_ASSERT_EQUAL(Admit::QueueFull, l.admit(Kind::Point, false));
  Record r;
  bool said = false;
  while (l.takeRecord(r))
    said = said || (std::string(r.status) == "dropped" && std::string(r.reason) == "queue_full");
  TEST_ASSERT_TRUE(said);
  TEST_ASSERT_EQUAL_UINT32(1, l.status(0).dropped);
}

static void changing_the_address_drops_what_was_queued_for_the_old_one() {
  Link l;
  joined(l);
  point(l, 'A');
  point(l, 'B');
  l.configure(true, 0);
  TEST_ASSERT_EQUAL(0, (int)l.queued());
  TEST_ASSERT_EQUAL(Phase::Waiting, l.phase());
  TEST_ASSERT_EQUAL(2, countRecords(l, "dropped"));
}

static void an_id_that_could_not_be_stored_is_not_sent() {
  Link l;
  joined(l);
  point(l, 'A');
  Request r = l.next(0, 0);
  l.notPersisted(r, 0, 0);
  TEST_ASSERT_EQUAL(1, (int)l.queued());
  TEST_ASSERT_EQUAL(Request::Type::None, l.next(999, 0).type);
  TEST_ASSERT_EQUAL_UINT32(r.event.eventId, l.next(1000, 0).event.eventId);
}

static void the_log_records_carry_no_text_from_the_answer() {
  Link l;
  joined(l);
  point(l, 'A');
  l.answered(l.next(0, 0), 400, "{\"error\":\"<script>evil</script>\"}", 0, 0);
  Record r;
  while (l.takeRecord(r)) {
    TEST_ASSERT_TRUE(std::string(r.reason).find("evil") == std::string::npos);
    TEST_ASSERT_TRUE(std::string(r.status).find("evil") == std::string::npos);
  }
}

static void the_status_shows_the_match_the_queue_and_the_last_error() {
  Link l;
  joined(l);
  point(l, 'A');
  l.answered(l.next(0, 0), 503, "", 0, 0);
  const std::string j = report::statusJson(l.status(0), "http://10.0.0.5:8080", SRC, 101, false);
  TEST_ASSERT_TRUE(j.find("\"phase\":\"joined\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"match_id\":\"m-1\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"queued\":1") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"last_error\":\"unavailable\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"last_http\":503") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"url\":\"http://10.0.0.5:8080\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"reachable\":false") != std::string::npos);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(an_http_address_with_a_port_is_usable);
  RUN_TEST(a_trailing_slash_is_taken_off);
  RUN_TEST(https_is_refused_with_a_reason_rather_than_failing_later);
  RUN_TEST(a_local_name_is_refused_because_the_client_does_not_ask_mdns);
  RUN_TEST(an_address_with_a_path_is_refused);
  RUN_TEST(spaces_umlauts_and_control_characters_are_refused);
  RUN_TEST(a_port_that_is_not_one_is_refused);
  RUN_TEST(anything_but_a_plain_host_is_refused);
  RUN_TEST(the_state_of_an_ingest_answer_is_read);
  RUN_TEST(the_state_inside_an_error_body_is_read);
  RUN_TEST(an_error_without_a_state_is_no_state);
  RUN_TEST(a_demo_is_recognised_by_its_flag);
  RUN_TEST(names_with_escapes_and_umlauts_come_through_as_utf8);
  RUN_TEST(a_half_read_answer_is_not_taken_for_a_state);
  RUN_TEST(an_answer_that_does_not_hold_together_is_refused);
  RUN_TEST(after_a_change_of_ends_the_points_follow_the_players);
  RUN_TEST(the_serve_comes_from_zaehlwerk_and_the_rotation_is_seeded_to_match);
  RUN_TEST(during_the_serve_rally_the_board_keeps_its_own_serve);
  RUN_TEST(the_winner_is_named_by_half);
  RUN_TEST(an_awarded_rally_is_a_point_for_that_half);
  RUN_TEST(a_rally_awarded_to_nobody_is_sent_as_delta_zero);
  RUN_TEST(a_point_names_its_half_never_its_player);
  RUN_TEST(an_undo_carries_no_half_and_no_delta);
  RUN_TEST(a_source_with_a_quote_cannot_break_the_body);
  RUN_TEST(no_answer_and_gateway_errors_are_sent_again);
  RUN_TEST(every_5xx_is_sent_again_like_the_reference_mock);
  RUN_TEST(a_500_holds_the_queue_and_keeps_the_event_id);
  RUN_TEST(any_other_answer_is_final);
  RUN_TEST(the_wait_doubles_from_one_second_up_to_thirty);
  RUN_TEST(the_jitter_adds_at_most_half_again);
  RUN_TEST(a_full_queue_refuses_the_new_event_and_keeps_the_old);
  RUN_TEST(the_queue_keeps_its_order_across_the_wrap);
  RUN_TEST(without_an_address_the_link_asks_for_nothing_and_says_nothing);
  RUN_TEST(clearing_the_address_makes_it_quiet_again);
  RUN_TEST(a_configured_link_first_asks_for_the_running_match);
  RUN_TEST(with_no_match_running_the_board_waits_and_asks_again_after_two_seconds);
  RUN_TEST(a_demo_is_not_joined_and_nothing_is_sent_into_it);
  RUN_TEST(a_running_match_is_joined_and_its_score_handed_to_the_board);
  RUN_TEST(an_undo_on_the_phone_reaches_the_board_with_the_next_poll);
  RUN_TEST(a_new_match_on_the_page_is_joined_in_place_of_the_old_one);
  RUN_TEST(a_match_ended_on_the_page_sends_the_board_back_to_waiting);
  RUN_TEST(a_failed_poll_during_a_rollout_keeps_the_match);
  RUN_TEST(a_point_is_sent_with_the_match_and_its_own_event_id);
  RUN_TEST(a_lost_answer_sends_the_same_event_id_again);
  RUN_TEST(a_rollout_is_ridden_out_with_growing_waits_and_the_same_id);
  RUN_TEST(queued_events_leave_in_order_once_zaehlwerk_is_back);
  RUN_TEST(an_older_answer_does_not_take_back_a_point_queued_after_it);
  RUN_TEST(an_answer_is_not_shown_while_later_points_still_wait);
  RUN_TEST(a_refused_event_is_dropped_and_the_next_one_goes);
  RUN_TEST(a_match_over_on_the_page_drops_its_events_and_the_board_waits);
  RUN_TEST(a_detected_rally_during_warmup_is_not_sent_but_a_point_given_by_hand_is);
  RUN_TEST(a_detected_rally_decides_the_serve_rally);
  RUN_TEST(after_a_won_match_an_undo_still_goes_but_a_point_does_not);
  RUN_TEST(a_full_queue_is_said_in_the_log);
  RUN_TEST(changing_the_address_drops_what_was_queued_for_the_old_one);
  RUN_TEST(an_id_that_could_not_be_stored_is_not_sent);
  RUN_TEST(the_log_records_carry_no_text_from_the_answer);
  RUN_TEST(the_status_shows_the_match_the_queue_and_the_last_error);
  return UNITY_END();
}
