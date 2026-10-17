#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// Reporting to zaehlwerk-api, the rules (#17, #41, #66, #67, #69, ADR-0008).
//
// No Arduino underneath. The HTTP client, NVS, the task and the clock belong to
// src/ingest.cpp and src/reporter.cpp; what is sent, in which order, when it is
// sent again, what an answer means and when the board takes the score from
// zaehlwerk are here, where `pio test -e native` reaches them.
//
// The whole thing is opt-in. A Link that was never configured answers every
// question with "nothing to do": it asks for no request, queues nothing, writes
// no record and has no score to hand over. That is what keeps a board without
// a zaehlwerk address exactly the board it was before.
namespace report {

// Statuses that are not HTTP: the request never reached anybody. The same
// values as the check-in client in #64, so the two can share one HTTP path.
const int NO_WIFI = -2;    // the board is not on a network that leads anywhere
const int NO_ANSWER = -1;  // on a network, but no answer: refused, timed out

// --- the address (#66) ------------------------------------------------------

// Why an address was refused. None means it is usable.
enum class UrlProblem { None, Length, Chars, Scheme, Https, Host, Local, Port, Path };

// Checks an address typed on the board's page or given as ZW_API_URL:
// http://, a host (an IP or a name the router's DNS knows) and an optional
// port, nothing after it but an optional slash. An empty string is not checked
// here; it means "clear" to the caller.
UrlProblem checkUrl(const std::string &url);
// The address as it is stored and used: the trailing slash taken off.
std::string normaliseUrl(const std::string &url);
// What the page says about a refused address. German, ASCII-transliterated
// like the rest of the page.
const char *urlProblemText(UrlProblem p);

// --- what zaehlwerk says about a match --------------------------------------

const size_t MATCH_ID_MAX = 40;
const size_t PLAYER_NAME_MAX = 40;

// The part of zaehlwerk's scorer.State the board uses, plus the demo flag that
// only GET /matches/current carries. Players are 'a' and 'b'; halves are 'A'
// (left) and 'B' (right).
struct State {
  bool valid = false;
  std::string matchId;
  std::string players[2];
  int points[2] = {0, 0};
  int sets[2] = {0, 0};
  int setNumber = 1;
  char serving = 0;  // 'a', 'b', or 0 while the serve rally has not decided it
  bool complete = false;
  char winner = 0;     // 'a', 'b' or 0
  std::string phase;   // "warmup" | "serve_rally" | "playing" | "change_ends" | "complete"
  char ends[2] = {'a', 'b'};  // who stands at the left half, who at the right
  bool demo = false;
};

// Reads a state out of an answer: the state itself (2xx, GET /matches/...), or
// the "state" an error body carries ({"error": ..., "state": {...}}). False if
// there is none, or if what is there does not hold together — a match id that
// is not one, a player that is neither a nor b.
bool parseState(const std::string &body, State &out);

// The half a player stands at, resolved against the ends zaehlwerk reports.
char halfOf(const State &s, char player);
// The player standing at a half.
char playerAt(const State &s, char half);

// --- what the board shows ----------------------------------------------------

// zaehlwerk's state as the board's own variables hold it: by half, not by
// player. Points, serve and the end of a game per half; names and sets per
// player, with the half-to-player mapping zaehlwerk's ends say.
struct Board {
  int pointsA = 0, pointsB = 0;
  char serve = 'A';       // the half that serves next
  char firstServe = 'A';  // the half lib/game's rotation starts from to arrive at serve
  bool over = false;
  char winner = ' ';             // the half of the winner, or ' '
  int sideToPlayer[2] = {0, 1};  // half A, half B -> 0 (player a) | 1 (player b)
  int sets[2] = {0, 0};          // by player
  int setNumber = 1;
};

// `currentServe` is kept while zaehlwerk has no server yet (the serve rally).
Board boardFrom(const State &s, char currentServe);

// What a finished rally becomes on the wire: a point for the half lib/game
// awarded it to, or delta 0 for a rally it awarded to nobody (#17), sent with
// the half the ball was last heard on, since the contract wants a side even
// for a hit that is not a point. half is 0 for an empty sequence: nothing to
// send.
struct Verdict {
  char half;
  uint8_t delta;
};
Verdict fromRally(char winner, const std::string &sequence);

// --- the events --------------------------------------------------------------

enum class Kind : uint8_t { Point, Undo };

// One ingest event, as it waits in the queue. The id is fixed when it is
// queued and never changes, however often it is sent (ADR-0002, #67).
struct Event {
  uint32_t eventId = 0;
  Kind kind = Kind::Point;
  char half = 0;      // 'A' | 'B', a point only
  uint8_t delta = 0;  // 0 | 1, a point only
  char matchId[MATCH_ID_MAX + 1] = {0};
  uint32_t queuedAt = 0;  // millis()
  uint32_t seq = 0;       // the how-manieth event this Link has queued
  uint16_t attempts = 0;
  // The board's own point and rally this event came from, for the diagnostic
  // log only: what lets the sink line a delivery up with the point it carried.
  // Never sent to zaehlwerk.
  uint32_t pointRef = 0;
  uint32_t rallyRef = 0;
};

// The body of POST /ingest/piezo. A point names its half, never its player:
// zaehlwerk owns the ends (its ADR-0005). An undo carries no half and no delta
// (its ADR-0012), and both carry the match the board joined.
std::string body(const char *source, const Event &e);

// "left" / "right"; nullptr for anything but 'A'/'B'.
const char *halfToken(char half);

// --- sending again (#69) -------------------------------------------------------

// What to do after an attempt.
enum class Next {
  Done,       // answered 2xx: delivered, or a duplicate of one that was
  Retry,      // nobody answered, or any 5xx (a rollout's gateway): same id, later
  MatchGone,  // 404/409: the match is over or gone; the event has nowhere to go
  Drop,       // any other answer (4xx, 3xx): zaehlwerk refused it, and would again
};
Next classify(int status);

// The wait before try number `failures` + 1, in milliseconds: 1 s doubling to
// 30 s, plus up to half again at random so boards that lost zaehlwerk
// together do not all come back in the same millisecond. `random` is any
// 32-bit random number.
const uint32_t RETRY_FIRST_MS = 1000;
const uint32_t RETRY_MAX_MS = 30000;
uint32_t backoffMs(uint32_t failures, uint32_t random);

// --- the queue (#17) -------------------------------------------------------------

// A fixed ring, oldest first. When it is full a new event is refused rather
// than an old one overwritten: what is in it is a gapless run of what
// happened, and zaehlwerk receiving a prefix of the match is easier to put
// right on the phone than a match with a hole somewhere in the middle.
class Queue {
 public:
  static const size_t CAPACITY = 64;

  bool push(const Event &e);
  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == CAPACITY; }
  size_t size() const { return size_; }
  Event &front() { return items_[head_]; }
  const Event &front() const { return items_[head_]; }
  void pop();
  void clear();

 private:
  Event items_[CAPACITY];
  size_t head_ = 0;
  size_t size_ = 0;
};

// --- the link -----------------------------------------------------------------

// Where the board stands with zaehlwerk.
enum class Phase : uint8_t {
  Off,       // no address: the board counts on its own, as before
  Waiting,   // an address, but no match running (or not reached yet)
  Demo,      // the running match is a demo; the board waits for a real one
  Joined,    // in a running match: points go there, the score comes from there
  Finished,  // the joined match is won; an undo can still take the match point back
};
const char *phaseName(Phase p);

// Whether a point or an undo goes into the queue.
enum class Admit {
  Queue,
  NotConfigured,  // no address
  NotJoined,      // no match to send it to
  SkippedPhase,   // a detected rally during warmup or change of ends
  MatchOver,      // a point after the match was won
  QueueFull,
};
const char *admitName(Admit a);

// Something for the diagnostic log. Static tokens and numbers, never text
// copied out of an answer.
struct Record {
  enum class Type : uint8_t { Ingest, Link } type = Type::Ingest;
  const char *status = "";  // ingest: queued|acked|retry|dropped; link: joined|...
  const char *reason = "";  // a token, or ""
  uint32_t eventId = 0;
  Kind kind = Kind::Point;
  char half = 0;
  uint8_t delta = 0;
  char matchId[MATCH_ID_MAX + 1] = {0};
  int http = 0;
  uint16_t attempt = 0;
  uint32_t queuedMs = 0;
  uint32_t waitMs = 0;
  uint32_t pointRef = 0;
  uint32_t rallyRef = 0;
};

// What the task should do next.
struct Request {
  enum class Type : uint8_t {
    None,  // nothing due; ask again in waitMs at the latest
    Join,  // GET /matches/current
    Send,  // POST /ingest/piezo with event
  } type = Type::None;
  Event event;
  uint32_t ticket = 0;  // which queued events the answer will already contain
  uint32_t waitMs = 0;
};

// What the page and /zaehlwerk show.
struct Status {
  bool configured = false;
  Phase phase = Phase::Off;
  bool reachable = false;
  std::string matchId;
  std::string players[2];
  int points[2] = {0, 0};
  int sets[2] = {0, 0};
  int setNumber = 0;
  std::string zwPhase;
  size_t queued = 0;
  uint32_t acked = 0;
  uint32_t dropped = 0;
  const char *lastError = "";
  int lastHttp = 0;
  uint32_t retryInMs = 0;
};

// The JSON of GET /zaehlwerk. The address and the source come from the caller.
std::string statusJson(const Status &s, const std::string &url, const char *source,
                       uint32_t lastEventId, bool mock);

// The rules of talking to zaehlwerk: join, queue, send in order, send again,
// and when the score shown on the board comes from zaehlwerk.
//
// Not thread-safe; the firmware holds a lock around it. Times are millis(),
// compared by difference, so the wrap after 49 days is harmless.
class Link {
 public:
  // How often to ask GET /matches/current: while waiting for a match, and
  // while joined with nothing to send, to see an undo from the phone, a won
  // set, or the match ending on the page.
  static const uint32_t POLL_MS = 2000;
  static const size_t RECORDS = 24;

  // An address was set (true) or cleared (false). A change of address while
  // events are queued drops them: they belong to a match on the other server.
  void configure(bool on, uint32_t now);
  bool configured() const { return phase_ != Phase::Off; }

  Phase phase() const { return phase_; }
  const State &match() const { return match_; }

  // Whether this point or undo goes to zaehlwerk. `manual` is a point given by
  // hand on the page, which is sent in every phase, as a button press would
  // be; a detected rally is not sent while zaehlwerk waits for the players to
  // confirm a warm-up or a change of ends, because a stray hit there would
  // confirm for them.
  Admit admit(Kind kind, bool manual) const;
  // Queues an event the caller admitted and gave an id. False if it does not
  // fit after all.
  bool push(Kind kind, char half, uint8_t delta, uint32_t eventId, uint32_t now,
            uint32_t pointRef = 0, uint32_t rallyRef = 0);
  // A record for an event that was not admitted, so the log says why a
  // rally went nowhere. Says nothing for NotConfigured or NotJoined.
  void refused(Admit why, Kind kind, char half, uint8_t delta, uint32_t pointRef = 0,
               uint32_t rallyRef = 0);

  Request next(uint32_t now, uint32_t random) const;
  // The answer to a request from next(). `status` is HTTP or NO_WIFI/NO_ANSWER.
  void answered(const Request &r, int status, const std::string &body, uint32_t now,
                uint32_t random);
  // The id could not be stored before the send, so it was not sent.
  void notPersisted(const Request &r, uint32_t now, uint32_t random);

  // The state zaehlwerk last answered, if it is newer than anything the board
  // queued since: then it is the score to show. Handed over once.
  bool takeAdoption(State &out);
  bool takeRecord(Record &out);
  uint32_t lostRecords() const { return lostRecords_; }

  Status status(uint32_t now) const;
  size_t queued() const { return queue_.size(); }

 private:
  void record(const Record &r);
  void recordEvent(const char *status, const char *reason, const Event &e, int http,
                   uint32_t now, uint32_t waitMs = 0);
  void recordLink(const char *status, const char *reason, int http, uint32_t waitMs = 0);
  void failed(int status, const char *why, uint32_t now, uint32_t random);
  void reached(uint32_t now);
  void join(const State &s, uint32_t ticket);
  void leave(const char *reason, Phase to);
  void dropMatch(const char *reason, int http, uint32_t now);
  void offer(const State &s, uint32_t ticket);

  Phase phase_ = Phase::Off;
  State match_;
  Queue queue_;
  uint32_t pushed_ = 0;  // events queued so far; the ticket counter

  // Consecutive failures, across joins and sends: an unreachable zaehlwerk is
  // unreachable for both.
  uint32_t failures_ = 0;
  uint32_t failingSince_ = 0;
  uint32_t notBefore_ = 0;
  uint32_t lastAsked_ = 0;
  bool askedOnce_ = false;
  const char *lastError_ = "";
  int lastHttp_ = 0;

  uint32_t acked_ = 0;
  uint32_t dropped_ = 0;

  State adoption_;
  uint32_t adoptionTicket_ = 0;
  bool haveAdoption_ = false;

  Record records_[RECORDS];
  size_t recHead_ = 0, recSize_ = 0;
  uint32_t lostRecords_ = 0;
};

}  // namespace report
