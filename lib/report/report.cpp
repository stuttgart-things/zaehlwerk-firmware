#include "report.h"

#include <stdio.h>
#include <string.h>

#include "game.h"

namespace report {

/* ---------------- the address ---------------- */

namespace {

const size_t URL_MAX = 96;

bool startsWith(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

bool endsWithNoCase(const std::string &s, const char *suffix) {
  const size_t n = strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[s.size() - n + i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != suffix[i]) return false;
  }
  return true;
}

}  // namespace

UrlProblem checkUrl(const std::string &url) {
  if (url.empty() || url.size() > URL_MAX) return UrlProblem::Length;
  for (char c : url)
    if ((unsigned char)c <= 0x20 || (unsigned char)c >= 0x7f) return UrlProblem::Chars;

  // TLS is left out on purpose for now: the client would cost tens of
  // kilobytes of heap per request and needs a certificate to check against.
  if (startsWith(url, "https://")) return UrlProblem::Https;
  if (!startsWith(url, "http://")) return UrlProblem::Scheme;

  const std::string rest = url.substr(7);
  size_t end = rest.find_first_of(":/");
  const std::string host = rest.substr(0, end);
  if (host.empty()) return UrlProblem::Host;
  for (char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-';
    if (!ok) return UrlProblem::Host;  // user@, [v6], ?, #, _ and the like
  }
  if (host.front() == '.' || host.front() == '-' || host.back() == '-')
    return UrlProblem::Host;
  // The HTTP client asks DNS, never mDNS, so a .local name never resolves. Said
  // now rather than as a "no answer" that looks like zaehlwerk being down.
  if (endsWithNoCase(host, ".local") || endsWithNoCase(host, ".local.")) return UrlProblem::Local;

  std::string tail = end == std::string::npos ? std::string() : rest.substr(end);
  if (!tail.empty() && tail[0] == ':') {
    size_t i = 1;
    long port = 0;
    while (i < tail.size() && tail[i] >= '0' && tail[i] <= '9') {
      port = port * 10 + (tail[i] - '0');
      if (port > 65535) return UrlProblem::Port;
      i++;
    }
    if (i == 1 || port == 0) return UrlProblem::Port;
    tail = tail.substr(i);
  }
  if (!tail.empty() && tail != "/") {
    // A port followed by anything but digits lands here too.
    return tail[0] == '/' ? UrlProblem::Path : UrlProblem::Port;
  }
  return UrlProblem::None;
}

std::string normaliseUrl(const std::string &url) {
  std::string u = url;
  if (!u.empty() && u.back() == '/') u.pop_back();
  return u;
}

const char *urlProblemText(UrlProblem p) {
  switch (p) {
    case UrlProblem::None:   return "";
    case UrlProblem::Length: return "Nicht gespeichert: 1 bis 96 Zeichen.";
    case UrlProblem::Chars:  return "Nicht gespeichert: keine Leerzeichen und keine Umlaute.";
    case UrlProblem::Scheme: return "Nicht gespeichert: die Adresse beginnt mit http://";
    case UrlProblem::Https:
      return "Nicht gespeichert: https kann das Board noch nicht, bitte http:// verwenden.";
    case UrlProblem::Host:   return "Nicht gespeichert: nach http:// fehlt ein gueltiger Rechnername oder eine IP.";
    case UrlProblem::Local:
      return "Nicht gespeichert: .local-Namen loest das Board nicht auf. Bitte die IP "
             "oder einen Namen aus dem DNS des Routers eintragen.";
    case UrlProblem::Port:   return "Nicht gespeichert: der Port ist keine Zahl von 1 bis 65535.";
    case UrlProblem::Path:
      return "Nicht gespeichert: nur Rechner und Port, ohne Pfad dahinter "
             "(also ohne /ingest/...).";
  }
  return "Nicht gespeichert.";
}

/* ---------------- a small JSON reader ---------------- */

// Just enough JSON to read one of zaehlwerk's answers: objects, arrays,
// strings with escapes, integers, booleans and null. Anything it does not need
// is skipped, nested up to a fixed depth. It never allocates more than the
// strings it hands back, and a body that is not JSON ends in "false", not in a
// crash.
namespace {

class Reader {
 public:
  explicit Reader(const std::string &s) : p_(s.data()), end_(s.data() + s.size()) {}

  void ws() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) p_++;
  }
  bool peek(char c) {
    ws();
    return p_ < end_ && *p_ == c;
  }
  bool eat(char c) {
    if (!peek(c)) return false;
    p_++;
    return true;
  }
  bool atEnd() {
    ws();
    return p_ >= end_;
  }

  bool str(std::string *out, size_t max) {
    if (!eat('"')) return false;
    std::string s;
    while (p_ < end_ && *p_ != '"') {
      char c = *p_++;
      if ((unsigned char)c < 0x20) return false;
      if (c != '\\') {
        s += c;
        continue;
      }
      if (p_ >= end_) return false;
      char e = *p_++;
      switch (e) {
        case '"': s += '"'; break;
        case '\\': s += '\\'; break;
        case '/': s += '/'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'n': s += '\n'; break;
        case 'r': s += '\r'; break;
        case 't': s += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t lo;
            if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u') return false;
            p_ += 2;
            if (!hex4(&lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(s, cp);
          break;
        }
        default: return false;
      }
    }
    if (p_ >= end_) return false;
    p_++;  // the closing quote
    if (out) {
      // Cut on a character boundary, never in the middle of a UTF-8 sequence.
      if (s.size() > max) {
        size_t n = max;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        s.resize(n);
      }
      *out = s;
    }
    return true;
  }

  bool integer(int *out) {
    ws();
    bool neg = false;
    if (p_ < end_ && *p_ == '-') {
      neg = true;
      p_++;
    }
    if (p_ >= end_ || *p_ < '0' || *p_ > '9') return false;
    long v = 0;
    while (p_ < end_ && *p_ >= '0' && *p_ <= '9') {
      if (v < 100000000) v = v * 10 + (*p_ - '0');
      p_++;
    }
    // A fraction or an exponent: not something zaehlwerk sends for a count.
    if (p_ < end_ && (*p_ == '.' || *p_ == 'e' || *p_ == 'E')) return false;
    if (out) *out = (int)(neg ? -v : v);
    return true;
  }

  bool boolean(bool *out) {
    ws();
    if (lit("true")) {
      if (out) *out = true;
      return true;
    }
    if (lit("false")) {
      if (out) *out = false;
      return true;
    }
    return false;
  }

  bool null() {
    ws();
    return lit("null");
  }

  bool skip(int depth = 0) {
    if (depth > 8) return false;
    ws();
    if (p_ >= end_) return false;
    switch (*p_) {
      case '"': return str(nullptr, 0);
      case '{': {
        p_++;
        if (eat('}')) return true;
        do {
          if (!str(nullptr, 0) || !eat(':') || !skip(depth + 1)) return false;
        } while (eat(','));
        return eat('}');
      }
      case '[': {
        p_++;
        if (eat(']')) return true;
        do {
          if (!skip(depth + 1)) return false;
        } while (eat(','));
        return eat(']');
      }
      case 't':
      case 'f': return boolean(nullptr);
      case 'n': return null();
      default: {
        // A number of any shape: skipped, not interpreted.
        const char *start = p_;
        while (p_ < end_ && (strchr("+-.eE", *p_) || (*p_ >= '0' && *p_ <= '9'))) p_++;
        return p_ > start;
      }
    }
  }

 private:
  bool lit(const char *w) {
    const size_t n = strlen(w);
    if ((size_t)(end_ - p_) < n || strncmp(p_, w, n) != 0) return false;
    p_ += n;
    return true;
  }

  bool hex4(uint32_t *out) {
    if (end_ - p_ < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p_++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
      else return false;
    }
    *out = v;
    return true;
  }

  static void utf8(std::string &s, uint32_t cp) {
    if (cp < 0x80) {
      s += (char)cp;
    } else if (cp < 0x800) {
      s += (char)(0xC0 | (cp >> 6));
      s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      s += (char)(0xE0 | (cp >> 12));
      s += (char)(0x80 | ((cp >> 6) & 0x3F));
      s += (char)(0x80 | (cp & 0x3F));
    } else {
      s += (char)(0xF0 | (cp >> 18));
      s += (char)(0x80 | ((cp >> 12) & 0x3F));
      s += (char)(0x80 | ((cp >> 6) & 0x3F));
      s += (char)(0x80 | (cp & 0x3F));
    }
  }

  const char *p_;
  const char *end_;
};

bool pairOfInts(Reader &r, int out[2]) {
  if (!r.eat('[')) return false;
  int v[2] = {0, 0};
  int n = 0;
  if (!r.eat(']')) {
    do {
      int x;
      if (!r.integer(&x)) return false;
      if (n < 2) v[n] = x;
      n++;
    } while (r.eat(','));
    if (!r.eat(']')) return false;
  }
  if (n != 2) return false;
  out[0] = v[0];
  out[1] = v[1];
  return true;
}

bool pairOfStrings(Reader &r, std::string out[2], size_t max) {
  if (!r.eat('[')) return false;
  std::string v[2];
  int n = 0;
  if (!r.eat(']')) {
    do {
      std::string x;
      if (!r.str(&x, max)) return false;
      if (n < 2) v[n] = x;
      n++;
    } while (r.eat(','));
    if (!r.eat(']')) return false;
  }
  if (n != 2) return false;
  out[0] = v[0];
  out[1] = v[1];
  return true;
}

// "a" -> 'a', "b" -> 'b', "" -> 0, anything else -> '?'
char player(const std::string &s) {
  if (s.empty()) return 0;
  if (s == "a") return 'a';
  if (s == "b") return 'b';
  return '?';
}

bool matchIdUsable(const std::string &id) {
  if (id.empty() || id.size() > MATCH_ID_MAX) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

// Reads the fields of one object. `nested` is filled when the object carries a
// "state" object (an error body); `sawState` says whether it did.
bool readObject(Reader &r, State &s, bool *sawMatch, int depth) {
  if (depth > 1 || !r.eat('{')) return false;
  if (r.eat('}')) return true;
  do {
    std::string key;
    if (!r.str(&key, 32) || !r.eat(':')) return false;
    if (key == "match_id") {
      if (r.null()) continue;
      if (!r.str(&s.matchId, MATCH_ID_MAX + 1)) return false;
      *sawMatch = true;
    } else if (key == "players") {
      if (!pairOfStrings(r, s.players, PLAYER_NAME_MAX)) return false;
    } else if (key == "points") {
      if (!pairOfInts(r, s.points)) return false;
    } else if (key == "sets") {
      if (!pairOfInts(r, s.sets)) return false;
    } else if (key == "set_number") {
      if (!r.integer(&s.setNumber)) return false;
    } else if (key == "serving") {
      std::string v;
      if (!r.str(&v, 4)) return false;
      s.serving = player(v);
    } else if (key == "complete") {
      if (!r.boolean(&s.complete)) return false;
    } else if (key == "winner") {
      std::string v;
      if (!r.str(&v, 4)) return false;
      s.winner = player(v);
    } else if (key == "phase") {
      if (!r.str(&s.phase, 16)) return false;
    } else if (key == "ends") {
      std::string e[2];
      if (!pairOfStrings(r, e, 4)) return false;
      s.ends[0] = player(e[0]);
      s.ends[1] = player(e[1]);
    } else if (key == "demo") {
      if (!r.boolean(&s.demo)) return false;
    } else if (key == "state") {
      if (r.null()) continue;
      State inner;
      bool innerMatch = false;
      if (!readObject(r, inner, &innerMatch, depth + 1)) return false;
      if (innerMatch) {
        s = inner;
        *sawMatch = true;
      }
    } else {
      if (!r.skip()) return false;
    }
  } while (r.eat(','));
  return r.eat('}');
}

}  // namespace

bool parseState(const std::string &body, State &out) {
  Reader r(body);
  State s;
  bool sawMatch = false;
  if (!readObject(r, s, &sawMatch, 0) || !r.atEnd() || !sawMatch) return false;

  // What does not hold together is not used: a board that took a score from a
  // half-read answer would show it as the truth.
  if (!matchIdUsable(s.matchId)) return false;
  if (s.serving == '?' || s.winner == '?') return false;
  const bool endsOk = (s.ends[0] == 'a' && s.ends[1] == 'b') || (s.ends[0] == 'b' && s.ends[1] == 'a');
  if (!endsOk) return false;
  for (int i = 0; i < 2; i++)
    if (s.points[i] < 0 || s.points[i] > 999 || s.sets[i] < 0 || s.sets[i] > 99) return false;
  if (s.setNumber < 1 || s.setNumber > 99) return false;

  s.valid = true;
  out = s;
  return true;
}

char halfOf(const State &s, char player) { return s.ends[0] == player ? 'A' : 'B'; }
char playerAt(const State &s, char half) { return half == 'A' ? s.ends[0] : s.ends[1]; }

Board boardFrom(const State &s, char currentServe) {
  Board b;
  const int a = s.ends[0] == 'b' ? 1 : 0;  // index of the player at the left half
  b.sideToPlayer[0] = a;
  b.sideToPlayer[1] = 1 - a;
  b.pointsA = s.points[a];
  b.pointsB = s.points[1 - a];
  b.serve = s.serving ? halfOf(s, s.serving) : (currentServe == 'B' ? 'B' : 'A');
  // lib/game turns a first server and a score into whoever serves now. Picking
  // the first server that arrives at zaehlwerk's answer keeps the board's own
  // rotation right for the points it counts before the next answer.
  b.firstServe = game::aufschlagFuer(b.pointsA, b.pointsB, 'A') == b.serve ? 'A' : 'B';
  b.over = s.complete;
  b.winner = (s.complete && s.winner) ? halfOf(s, s.winner) : ' ';
  b.sets[0] = s.sets[0];
  b.sets[1] = s.sets[1];
  b.setNumber = s.setNumber;
  return b;
}

Verdict fromRally(char winner, const std::string &sequence) {
  if (sequence.empty()) return {0, 0};
  if (winner == 'A' || winner == 'B') return {winner, 1};
  const char last = sequence.back();
  return {last == 'B' ? 'B' : 'A', 0};
}

/* ---------------- events ---------------- */

const char *halfToken(char half) {
  if (half == 'A') return "left";
  if (half == 'B') return "right";
  return nullptr;
}

namespace {

// Escapes for a JSON string. Everything this file writes is either a token of
// its own or a value it has already checked, but a source from a build flag is
// neither, so it goes through here too.
std::string esc(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((unsigned char)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(unsigned char)c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

std::string body(const char *source, const Event &e) {
  std::string b = "{\"match_id\":\"" + esc(e.matchId) + "\",\"source\":\"" +
                  esc(source ? source : "") + "\"";
  if (e.kind == Kind::Undo) {
    b += ",\"event_id\":" + std::to_string(e.eventId) + ",\"action\":\"undo\"}";
    return b;
  }
  const char *half = halfToken(e.half);
  b += std::string(",\"half\":\"") + (half ? half : "") + "\"";
  b += ",\"delta\":" + std::to_string(e.delta ? 1 : 0);
  b += ",\"event_id\":" + std::to_string(e.eventId) + "}";
  return b;
}

/* ---------------- sending again ---------------- */

Next classify(int status) {
  if (status >= 200 && status < 300) return Next::Done;
  if (status == NO_ANSWER || status == NO_WIFI) return Next::Retry;
  // Any 5xx: a rollout, where the gateway in front of zaehlwerk answers 502,
  // 503 or 504 while zaehlwerk does not (#69), or zaehlwerk's own 500. Same id
  // again, and if zaehlwerk did take the first one, the second is a duplicate
  // (ADR-0002). The zaehlwerk piezo mock, the reference client, does the same.
  if (status >= 500 && status <= 599) return Next::Retry;
  if (status == 404 || status == 409) return Next::MatchGone;
  return Next::Drop;
}

uint32_t backoffMs(uint32_t failures, uint32_t random) {
  uint32_t wait = RETRY_FIRST_MS;
  for (uint32_t i = 1; i < failures && wait < RETRY_MAX_MS; i++) wait *= 2;
  if (wait > RETRY_MAX_MS) wait = RETRY_MAX_MS;
  return wait + random % (wait / 2 + 1);
}

/* ---------------- the queue ---------------- */

bool Queue::push(const Event &e) {
  if (full()) return false;
  items_[(head_ + size_) % CAPACITY] = e;
  size_++;
  return true;
}

void Queue::pop() {
  if (empty()) return;
  head_ = (head_ + 1) % CAPACITY;
  size_--;
}

void Queue::clear() {
  head_ = 0;
  size_ = 0;
}

/* ---------------- the link ---------------- */

const char *phaseName(Phase p) {
  switch (p) {
    case Phase::Off:      return "off";
    case Phase::Waiting:  return "waiting";
    case Phase::Demo:     return "demo";
    case Phase::Joined:   return "joined";
    case Phase::Finished: return "finished";
  }
  return "off";
}

const char *admitName(Admit a) {
  switch (a) {
    case Admit::Queue:         return "queued";
    case Admit::NotConfigured: return "not_configured";
    case Admit::NotJoined:     return "not_joined";
    case Admit::SkippedPhase:  return "phase";
    case Admit::MatchOver:     return "match_over";
    case Admit::QueueFull:     return "queue_full";
  }
  return "";
}

namespace {

bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

uint32_t until(uint32_t now, uint32_t at) { return due(now, at) ? 0 : at - now; }

void copyId(char *dst, const std::string &id) {
  strncpy(dst, id.c_str(), MATCH_ID_MAX);
  dst[MATCH_ID_MAX] = '\0';
}

// zaehlwerk resolves a press against these phases (its ADR-0005): in warmup and
// change_ends a press from a half confirms that side, it does not score.
bool confirming(const std::string &phase) { return phase == "warmup" || phase == "change_ends"; }

}  // namespace

void Link::configure(bool on, uint32_t now) {
  const bool was = configured();
  if (was) {
    // Whatever is queued belongs to a match on the server that was there
    // before. Sending it to another one would score into a stranger's match.
    while (!queue_.empty()) {
      recordEvent("dropped", on ? "address_changed" : "address_cleared", queue_.front(), 0, now);
      dropped_++;
      queue_.pop();
    }
  }
  match_ = State();
  haveAdoption_ = false;
  failures_ = 0;
  lastError_ = "";
  lastHttp_ = 0;
  askedOnce_ = false;
  notBefore_ = now;
  phase_ = on ? Phase::Waiting : Phase::Off;
}

Admit Link::admit(Kind kind, bool manual) const {
  switch (phase_) {
    case Phase::Off: return Admit::NotConfigured;
    case Phase::Waiting:
    case Phase::Demo: return Admit::NotJoined;
    case Phase::Finished:
      // The match point is the mis-press an undo after a won match exists for
      // (zaehlwerk ADR-0012). A point has nowhere to go.
      if (kind == Kind::Point) return Admit::MatchOver;
      break;
    case Phase::Joined:
      if (kind == Kind::Point && !manual && confirming(match_.phase)) return Admit::SkippedPhase;
      break;
  }
  if (queue_.full()) return Admit::QueueFull;
  return Admit::Queue;
}

bool Link::push(Kind kind, char half, uint8_t delta, uint32_t eventId, uint32_t now,
                uint32_t pointRef, uint32_t rallyRef) {
  if (phase_ != Phase::Joined && phase_ != Phase::Finished) return false;
  Event e;
  e.eventId = eventId;
  e.kind = kind;
  e.half = kind == Kind::Point ? half : 0;
  e.delta = kind == Kind::Point ? (delta ? 1 : 0) : 0;
  copyId(e.matchId, match_.matchId);
  e.queuedAt = now;
  e.seq = pushed_ + 1;
  e.pointRef = pointRef;
  e.rallyRef = rallyRef;
  if (!queue_.push(e)) return false;
  pushed_++;
  // Anything zaehlwerk said before this is older than the board now is.
  haveAdoption_ = false;
  recordEvent("queued", "", e, 0, now);
  return true;
}

void Link::refused(Admit why, Kind kind, char half, uint8_t delta, uint32_t pointRef,
                   uint32_t rallyRef) {
  if (why == Admit::Queue || why == Admit::NotConfigured || why == Admit::NotJoined) return;
  Event e;
  e.kind = kind;
  e.half = half;
  e.delta = delta;
  e.pointRef = pointRef;
  e.rallyRef = rallyRef;
  copyId(e.matchId, match_.matchId);
  if (why == Admit::QueueFull) dropped_++;
  recordEvent("dropped", admitName(why), e, 0, 0);
}

Request Link::next(uint32_t now, uint32_t random) const {
  (void)random;
  Request r;
  if (phase_ == Phase::Off) {
    r.waitMs = 1000;
    return r;
  }
  if (!due(now, notBefore_)) {
    r.waitMs = until(now, notBefore_);
    return r;
  }
  if (!queue_.empty()) {
    r.type = Request::Type::Send;
    r.event = queue_.front();
    r.ticket = queue_.front().seq;
    return r;
  }
  const uint32_t pollAt = lastAsked_ + POLL_MS;
  if (askedOnce_ && failures_ == 0 && !due(now, pollAt)) {
    r.waitMs = until(now, pollAt);
    return r;
  }
  r.type = Request::Type::Join;
  r.ticket = pushed_;
  return r;
}

void Link::record(const Record &r) {
  if (recSize_ == RECORDS) {
    // The oldest goes. A line of log is lost, never an event.
    recHead_ = (recHead_ + 1) % RECORDS;
    recSize_--;
    lostRecords_++;
  }
  records_[(recHead_ + recSize_) % RECORDS] = r;
  recSize_++;
}

bool Link::takeRecord(Record &out) {
  if (recSize_ == 0) return false;
  out = records_[recHead_];
  recHead_ = (recHead_ + 1) % RECORDS;
  recSize_--;
  return true;
}

void Link::recordEvent(const char *status, const char *reason, const Event &e, int http,
                       uint32_t now, uint32_t waitMs) {
  Record r;
  r.type = Record::Type::Ingest;
  r.status = status;
  r.reason = reason;
  r.eventId = e.eventId;
  r.kind = e.kind;
  r.half = e.half;
  r.delta = e.delta;
  memcpy(r.matchId, e.matchId, sizeof(r.matchId));
  r.http = http;
  r.attempt = e.attempts;
  r.queuedMs = (now && e.queuedAt) ? now - e.queuedAt : 0;
  r.waitMs = waitMs;
  r.pointRef = e.pointRef;
  r.rallyRef = e.rallyRef;
  record(r);
}

void Link::recordLink(const char *status, const char *reason, int http, uint32_t waitMs) {
  Record r;
  r.type = Record::Type::Link;
  r.status = status;
  r.reason = reason;
  copyId(r.matchId, match_.matchId);
  r.http = http;
  r.attempt = (uint16_t)(failures_ > 0xffff ? 0xffff : failures_);
  r.waitMs = waitMs;
  record(r);
}

namespace {

const char *errorToken(int status) {
  switch (status) {
    case NO_WIFI:   return "no_wifi";
    case NO_ANSWER: return "no_answer";
    case 502:       return "bad_gateway";
    case 503:       return "unavailable";
    case 504:       return "gateway_timeout";
    case 400:       return "bad_request";
    case 404:       return "not_found";
    case 409:       return "conflict";
    case 500:       return "server_error";
    default:        return status >= 500 ? "server_error" : "http_error";
  }
}

}  // namespace

void Link::failed(int status, const char *why, uint32_t now, uint32_t random) {
  if (failures_ == 0) failingSince_ = now;
  failures_++;
  lastError_ = why;
  lastHttp_ = status > 0 ? status : 0;
  notBefore_ = now + backoffMs(failures_, random);
}

void Link::reached(uint32_t now) {
  if (failures_ > 0) {
    // Said once, with how long it took, so a rollout reads as one gap in the
    // log rather than as a wall of retries with no end.
    recordLink("back", "", 0, now - failingSince_);
  }
  failures_ = 0;
  lastError_ = "";
  notBefore_ = now;
}

void Link::offer(const State &s, uint32_t ticket) {
  // Only an answer that already contains everything the board has queued is
  // the score to show. An older one would take back a point the board just
  // counted and give it back a moment later.
  if (ticket != pushed_ || !queue_.empty()) return;
  adoption_ = s;
  adoptionTicket_ = ticket;
  haveAdoption_ = true;
}

bool Link::takeAdoption(State &out) {
  if (!haveAdoption_ || adoptionTicket_ != pushed_) return false;
  out = adoption_;
  haveAdoption_ = false;
  return true;
}

void Link::join(const State &s, uint32_t ticket) {
  const bool same = (phase_ == Phase::Joined || phase_ == Phase::Finished) && s.matchId == match_.matchId;
  match_ = s;
  phase_ = s.complete ? Phase::Finished : Phase::Joined;
  if (!same) recordLink("joined", "", 200);
  offer(s, ticket);
}

void Link::leave(const char *reason, Phase to) {
  if (phase_ == Phase::Joined || phase_ == Phase::Finished) {
    recordLink("left", reason, 0);
  } else if (phase_ != to) {
    recordLink(to == Phase::Demo ? "demo" : "waiting", reason, 0);
  }
  if (to != Phase::Joined) match_ = State();
  phase_ = to;
  haveAdoption_ = false;
}

void Link::dropMatch(const char *reason, int http, uint32_t now) {
  const std::string gone = queue_.empty() ? std::string() : queue_.front().matchId;
  // Everything queued for the match that is gone goes with it. In order, so
  // the log reads like the queue did.
  while (!queue_.empty() && gone == queue_.front().matchId) {
    recordEvent("dropped", reason, queue_.front(), http, now);
    dropped_++;
    queue_.pop();
  }
}

void Link::answered(const Request &r, int status, const std::string &body, uint32_t now,
                    uint32_t random) {
  if (phase_ == Phase::Off || r.type == Request::Type::None) return;

  if (r.type == Request::Type::Join) {
    lastAsked_ = now;
    askedOnce_ = true;
    State s;
    if (status == 200 && parseState(body, s)) {
      reached(now);
      if (s.demo) {
        // A demo is any match zaehlwerk-pong started: its bots, the
        // screensaver, a game in the browser (zaehlwerk ADR-0008). The board
        // waits as it does for an empty table.
        leave("demo", Phase::Demo);
      } else {
        join(s, r.ticket);
      }
      return;
    }
    if (status == 404) {
      reached(now);
      // A won match is no longer the running one, so it disappears from
      // /matches/current. The board stays with it, because the undo of the
      // match point has to name it, until another match starts.
      if (phase_ != Phase::Finished) leave("no_match", Phase::Waiting);
      return;
    }
    // A rollout, no network, or an answer that does not read. The board keeps
    // the match it has and the score it shows, and asks again (#69).
    const char *why = status == 200 ? "bad_answer" : errorToken(status);
    const bool first = failures_ == 0;
    failed(status, why, now, random);
    if (first) recordLink("unreachable", why, status > 0 ? status : 0, notBefore_ - now);
    return;
  }

  // A send. The head of the queue is the event that went, because nothing but
  // answered() takes it off.
  if (queue_.empty() || queue_.front().seq != r.event.seq) return;
  Event &e = queue_.front();
  e.attempts++;

  switch (classify(status)) {
    case Next::Done: {
      State s;
      const bool have = parseState(body, s);
      recordEvent("acked", "", e, status, now);
      acked_++;
      queue_.pop();
      reached(now);
      if (have && s.matchId == match_.matchId) {
        // The answer is as fresh as a poll would be, so the next poll waits.
        lastAsked_ = now;
        askedOnce_ = true;
        match_ = s;
        if (s.complete) phase_ = Phase::Finished;
        else if (phase_ == Phase::Finished) phase_ = Phase::Joined;  // the match point taken back
        offer(s, r.ticket);
      }
      return;
    }
    case Next::Retry: {
      failed(status, errorToken(status), now, random);
      recordEvent("retry", errorToken(status), e, status > 0 ? status : 0, now, notBefore_ - now);
      return;
    }
    case Next::MatchGone: {
      reached(now);
      State s;
      const bool have = parseState(body, s);
      const bool wonHere = have && s.complete && s.matchId == match_.matchId;
      dropMatch("match_over", status, now);
      if (wonHere) {
        match_ = s;
        phase_ = Phase::Finished;
        offer(s, pushed_);
      } else if (phase_ == Phase::Joined || phase_ == Phase::Finished) {
        leave("match_over", Phase::Waiting);
      }
      return;
    }
    case Next::Drop: {
      // zaehlwerk answered and refused. The same body would be refused again,
      // so it goes, and the next one is not held up behind it.
      lastError_ = errorToken(status);
      lastHttp_ = status;
      recordEvent("dropped", "refused", e, status, now);
      dropped_++;
      queue_.pop();
      failures_ = 0;
      notBefore_ = now;
      return;
    }
  }
}

void Link::notPersisted(const Request &r, uint32_t now, uint32_t random) {
  if (queue_.empty() || queue_.front().seq != r.event.seq) return;
  // Sending an id that is not stored could reuse it after a reboot, and
  // zaehlwerk would take the next point for a duplicate. So it waits.
  failed(0, "nvs", now, random);
  recordEvent("retry", "nvs", queue_.front(), 0, now, notBefore_ - now);
}

Status Link::status(uint32_t now) const {
  Status s;
  s.configured = configured();
  s.phase = phase_;
  s.reachable = configured() && askedOnce_ && failures_ == 0;
  s.matchId = match_.matchId;
  s.players[0] = match_.players[0];
  s.players[1] = match_.players[1];
  s.points[0] = match_.points[0];
  s.points[1] = match_.points[1];
  s.sets[0] = match_.sets[0];
  s.sets[1] = match_.sets[1];
  s.setNumber = match_.valid ? match_.setNumber : 0;
  s.zwPhase = match_.phase;
  s.queued = queue_.size();
  s.acked = acked_;
  s.dropped = dropped_;
  s.lastError = lastError_;
  s.lastHttp = lastHttp_;
  s.retryInMs = failures_ ? until(now, notBefore_) : 0;
  return s;
}

std::string statusJson(const Status &s, const std::string &url, const char *source,
                       uint32_t lastEventId, bool mock) {
  std::string j = "{\"configured\":";
  j += s.configured ? "true" : "false";
  j += ",\"url\":\"" + esc(url) + "\"";
  j += ",\"source\":\"" + esc(source ? source : "") + "\"";
  j += std::string(",\"phase\":\"") + phaseName(s.phase) + "\"";
  j += std::string(",\"reachable\":") + (s.reachable ? "true" : "false");
  j += std::string(",\"mock\":") + (mock ? "true" : "false");
  j += ",\"match_id\":\"" + esc(s.matchId) + "\"";
  j += ",\"players\":[\"" + esc(s.players[0]) + "\",\"" + esc(s.players[1]) + "\"]";
  j += ",\"points\":[" + std::to_string(s.points[0]) + "," + std::to_string(s.points[1]) + "]";
  j += ",\"sets\":[" + std::to_string(s.sets[0]) + "," + std::to_string(s.sets[1]) + "]";
  j += ",\"set\":" + std::to_string(s.setNumber);
  j += ",\"zw_phase\":\"" + esc(s.zwPhase) + "\"";
  j += ",\"queued\":" + std::to_string(s.queued);
  j += ",\"acked\":" + std::to_string(s.acked);
  j += ",\"dropped\":" + std::to_string(s.dropped);
  j += std::string(",\"last_error\":\"") + s.lastError + "\"";
  j += ",\"last_http\":" + std::to_string(s.lastHttp);
  j += ",\"retry_in_ms\":" + std::to_string(s.retryInMs);
  j += ",\"event_id\":" + std::to_string(lastEventId);
  j += "}";
  return j;
}

}  // namespace report
