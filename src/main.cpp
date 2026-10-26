/*
  Zählwerk — piezo firmware
  --------------------------------------------------------------
  Carried over from sketches/stufe2-zaehlwerk-mvp with the behaviour unchanged:
  two piezos on one ESP32, the full counting logic, and the scoreboard served
  from an access point the board carries itself. No router needed.

  The sketch stays under sketches/ as the record. From here it is built and
  flashed with PlatformIO:

      task flash          (or: pio run -e piezo -t upload)
      task monitor

  Hardware:
    piezo half A -> GPIO 34   (each with 1 MΩ ∥, 100 kΩ series, 2× 1N4148)
    piezo half B -> GPIO 35

  Use:
    wifi "Zaehlwerk", password from secrets.ini, then http://192.168.4.1

  Identifiers and the page stay German: the identifiers because this is a port
  and they should still line up with the sketch line for line, the page because
  a player reads it. Comments and serial output are English, like the rest of
  the repository.

  How the firmware is laid out:
    core 0  sensor task, sweeps both channels and reports events
    core 1  web server and game logic
  That split matters — otherwise the web server swallows bounces. Note that the
  sides are the wrong way round and ADR-0003 reverses them: the wifi task lives
  on core 0 too. Until #10 lands, this is the sketch's arrangement.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <string>

#include "buttons.h"
#include "config.h"
#include "diag.h"
#include "fob.h"
#include "game.h"
#include "ingest.h"
#include "mock.h"
#include "net.h"
#include "ota.h"
#include "reporter.h"
#include "version.h"

/* ================= configuration ================= */
const int  PIN_A = ZW_PIN_A;
const int  PIN_B = ZW_PIN_B;
const char *AP_PREFIX = ZW_AP_SSID;   // the chip id is appended, see net.cpp
const char *AP_PASS   = ZW_AP_PASS;

volatile int schwelleA   = 300;
volatile int schwelleB   = 300;
volatile int rallyTimeout = 1500;   // ms Stille = Ballwechsel vorbei

// Everything above log_threshold is written to the log, whether or not it
// counts. Below the counting threshold on purpose: the crossings that were
// thrown away are the half the scoreboard never mentions.
volatile int logSchwelleA = 120;
volatile int logSchwelleB = 120;

// How much louder one channel has to be before the side is more than a guess.
// It classifies today, it does not yet decide — see diag::Hit::counted.
volatile int clearRatioPromille = 1800;

// Where a press on a button becomes long (take back) and very long (new game).
// The page's virtual buttons are timed with these, and the radio buttons (#58)
// will be told them. See lib/buttons.
volatile int tasteLangMs     = 1000;
volatile int tasteSehrLangMs = 3000;

const int SPERRE_MS  = 60;          // Nachklingen
const int FENSTER_MS = 30;          // Vergleichsfenster zwischen den Kanälen

/* ================= sensor task ================= */
struct Treffer { char seite; int spitzeA; int spitzeB; uint32_t t; };
QueueHandle_t queue;

// A handle, to suspend sampling while an update is written, and a counter that
// lets anything outside see the task is really running. The rollback needs
// both: with no sign of life from the sensor task, an image that boots but can
// no longer measure would count as "started cleanly".
// The bring-up measurement from issue #35, step 2. Quiet sampling costs a
// vTaskDelay(1) per reading, so the channels are looked at every 1.3 ms while a
// piezo transient is over in a few. Triggering needs an instantaneous sample
// above the threshold, so the peak is caught only by luck — which is what the
// finger taps showed: single samples of 65, 105, 110 from taps whose real peaks
// were never measured, because no window opens below the threshold.
//
// With ZW_CONTINUOUS_SAMPLING the loop yields instead of sleeping and the task
// moves to core 1, where nothing else runs. Core 1's idle task is not watched by
// the watchdog in this build, so a tight loop there is survivable; on core 0 it
// would starve wifi and lwIP.
// ZW_SENSOR_CORE is in include/config.h, because the link to zaehlwerk-api and
// the fob reader have to know which core to stay off.
// How many readings between one tick of sleep. A bare taskYIELD() here starved
// core 1's idle task: the image booted, confirmed itself valid after the ten
// second probation and then crash-looped every thirty-five seconds — past the
// point where the rollback could still help. FreeRTOS needs the idle task to run.
// Sixty-four readings is about eleven milliseconds of sampling for one
// millisecond of sleep, so roughly 5 kHz survives and the core stays alive.
#ifndef ZW_YIELD_EVERY
#define ZW_YIELD_EVERY 64
#endif

#if ZW_CONTINUOUS_SAMPLING
inline void sensorYield() {
  static uint16_t seit = 0;
  if (++seit >= ZW_YIELD_EVERY) { seit = 0; vTaskDelay(1); }
  else                          taskYIELD();
}
#else
inline void sensorYield() { vTaskDelay(1); }
#endif
#define ZW_SENSOR_YIELD() sensorYield()

TaskHandle_t sensorTaskHandle = nullptr;
volatile uint32_t sensorTicks = 0;

// The quiet level of each channel, tracked while nothing is happening. A peak
// means little without it — the channels sit at different levels and drift.
volatile int baselineA = 0, baselineB = 0;

// What the channels are doing right now, for the bring-up at the table. The
// baseline alone does not answer whether a channel sits still or jitters, and
// that is the first thing to know: a lead acting as an antenna looks exactly
// like a signal. Min and max span the window since the last read of /diag, so
// each poll measures its own stretch of time rather than all of history.
volatile int letzteA = 0, letzteB = 0;
volatile int minA = 4095, maxA = 0, minB = 4095, maxB = 0;

// Ids. Every hit belongs to a rally, every point to the rally it ended, so a
// correction later can point at one thing rather than at a span of time.
uint32_t rallyId = 0, pointId = 0;
// Read by the sensor task now, written by the main loop, so volatile: a plain
// bool may sit in a register on one core while the other changes it.
volatile bool rallyOffen = false;

// The pre-trigger ring: what the sampler managed to take before the crossing.
// At one sample per millisecond that is not a curve yet; #10 is what makes it
// one. The shape is right either way, so the sink does not change with it.
// The ring keeps the full timestamp; the event stores it relative to the
// crossing once that is known.
struct Vorlauf { uint32_t tUs; int16_t a, b; };
Vorlauf vorlauf[diag::PRE_SAMPLES];
size_t vorlaufKopf = 0;
uint32_t vorlaufFuell = 0;

void sensorTask(void *) {
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  uint32_t sperreBis = 0;

  for (;;) {
    sensorTicks++;

    const uint32_t jetztUs = micros();
    const bool ausMock = mock::on();
    int a = ausMock ? mock::read(true, jetztUs)  : analogRead(PIN_A);
    int b = ausMock ? mock::read(false, jetztUs) : analogRead(PIN_B);

    letzteA = a; letzteB = b;
    if (a < minA) minA = a;
    if (a > maxA) maxA = a;
    if (b < minB) minB = b;
    if (b > maxB) maxB = b;

    vorlauf[vorlaufKopf] = { jetztUs, (int16_t)a, (int16_t)b };
    vorlaufKopf = (vorlaufKopf + 1) % diag::PRE_SAMPLES;
    vorlaufFuell++;

    const bool sperre = millis() < sperreBis;
    const bool uebertritt = (a >= logSchwelleA || b >= logSchwelleB);

    if (!uebertritt) {
      // Quiet: let the baselines follow, slowly enough that a hit does not
      // drag them along.
      baselineA += (a - baselineA) / 64;
      baselineB += (b - baselineB) / 64;
      ZW_SENSOR_YIELD();
      continue;
    }

    // What this crossing was meant to be, read now rather than after the peak
    // window: a generated event is over in twelve milliseconds and the window
    // runs for thirty, so asking afterwards always found nothing.
    const char sollSeite = mock::intendedSide();
    const char *sollTyp = mock::intendedType();

    const bool zaehlt = !sperre && (a >= schwelleA || b >= schwelleB);

    if (!zaehlt) {
      // Logged, but nothing else changes. No peak window here on purpose: it
      // would cost thirty milliseconds of blindness that the old code did not
      // spend, and this change is meant to measure detection, not alter it.
      diag::Hit h{};
      // The rally this hit will belong to, not the one that just ended. The task
      // that counts rallies is the main loop, and it only opens the next one when
      // it dequeues this hit — so stamping the current id here filed the first
      // bounce of every rally under its predecessor. Measured over 150 rallies:
      // 271 counted hits landed after the end of the rally they claimed to be in,
      // which makes the log unusable for exactly the question it exists to answer.
      h.rallyId = rallyOffen ? rallyId : rallyId + 1;
      h.side = ' ';
      h.decision = sperre ? diag::Decision::Deadtime : diag::Decision::BelowThreshold;
      h.intendedSide = sollSeite;
      h.intendedType = sollTyp;
      h.peakA = a; h.peakB = b;
      h.baselineA = baselineA; h.baselineB = baselineB;
      h.crossAUs = a >= logSchwelleA ? 0 : -1;
      h.crossBUs = b >= logSchwelleB ? 0 : -1;
      h.ratio = 0;
      h.counted = false;
      h.tUs = jetztUs;
      h.preUs = 0;
      h.sampleCount = 0;
      diag::hit(h);
      ZW_SENSOR_YIELD();
      continue;
    }

    {
      // Follow both channels for FENSTER_MS and collect the peaks. The samples
      // are recorded on the way past — the loop is unchanged otherwise.
      diag::Hit h{};
      uint16_t n = 0;
      for (size_t i = 0; i < diag::PRE_SAMPLES && n < diag::CAPTURE_SAMPLES; i++) {
        size_t k = (vorlaufKopf + i) % diag::PRE_SAMPLES;
        if (vorlaufFuell < diag::PRE_SAMPLES && k >= vorlaufFuell) continue;
        h.samples[n] = { (int32_t)(vorlauf[k].tUs - jetztUs),
                         vorlauf[k].a, vorlauf[k].b };
        n++;
      }

      uint32_t start = millis();
      int spA = a, spB = b;
      int32_t kreuzA = a >= schwelleA ? 0 : -1;
      int32_t kreuzB = b >= schwelleB ? 0 : -1;
      while (millis() - start < (uint32_t)FENSTER_MS) {
        uint32_t tUs = micros();
        int va = ausMock ? mock::read(true, tUs)  : analogRead(PIN_A);
        int vb = ausMock ? mock::read(false, tUs) : analogRead(PIN_B);
        if (va > spA) spA = va;
        if (vb > spB) spB = vb;
        if (kreuzA < 0 && va >= schwelleA) kreuzA = (int32_t)(tUs - jetztUs);
        if (kreuzB < 0 && vb >= schwelleB) kreuzB = (int32_t)(tUs - jetztUs);
        if (n < diag::CAPTURE_SAMPLES) {
          h.samples[n++] = { (int32_t)(tUs - jetztUs), (int16_t)va, (int16_t)vb };
        }
      }

      // Compare against each channel's own threshold — the two are never
      // exactly equally sensitive.
      float relA = (float)spA / (float)schwelleA;
      float relB = (float)spB / (float)schwelleB;

      Treffer t;
      t.t = millis();
      t.spitzeA = spA;
      t.spitzeB = spB;
      if (relA >= relB) { t.seite = 'A'; }
      else              { t.seite = 'B'; }

      const float gross = max(relA, relB), klein = min(relA, relB);
      const float verhaeltnis = klein > 0 ? gross / klein : 999.0f;
      const bool eindeutig = verhaeltnis * 1000.0f >= (float)clearRatioPromille;

      // The rally this hit will belong to, not the one that just ended. The task
      // that counts rallies is the main loop, and it only opens the next one when
      // it dequeues this hit — so stamping the current id here filed the first
      // bounce of every rally under its predecessor. Measured over 150 rallies:
      // 271 counted hits landed after the end of the rally they claimed to be in,
      // which makes the log unusable for exactly the question it exists to answer.
      h.rallyId = rallyOffen ? rallyId : rallyId + 1;
      h.side = t.seite;
      h.decision = eindeutig ? diag::Decision::Counted : diag::Decision::Ambiguous;
      h.intendedSide = sollSeite;
      h.intendedType = sollTyp;
      h.peakA = spA; h.peakB = spB;
      h.baselineA = baselineA; h.baselineB = baselineB;
      h.crossAUs = kreuzA; h.crossBUs = kreuzB;
      h.ratio = verhaeltnis;
      h.counted = true;   // unchanged: the louder channel wins, clear or not
      h.tUs = jetztUs;
      h.preUs = 0;
      h.sampleCount = n;
      diag::hit(h);

      xQueueSend(queue, &t, 0);
      sperreBis = millis() + SPERRE_MS;
    }
    ZW_SENSOR_YIELD();
  }
}

/* ================= game state ================= */
struct Eintrag { String folge; String urteil; String hinweis; };

int   punkteA = 0, punkteB = 0;
// Names belong to players, not to halves. A piezo knows which half it is under
// and can never know who is standing there — and between sets the players
// change ends while the sensor does not. So the mapping lives here and inverts
// on a change of ends (ADR-0006).
//
// Index 0 is player a, index 1 is player b. For doubles both names go in one
// field: "Pat & Chris". That is a display question, not a data model one.
String spieler[2];
int seiteZuSpieler[2] = {0, 1};   // half A -> a, half B -> b
int satzNummer = 1;
int saetze[2] = {0, 0};   // by player, not by half

// How far a simulated run goes. Endless is for watching the chain; one game and
// one match are for producing a session that looks like an evening.
enum class Simulation { Aus, Dauerhaft, EinSpiel, EinMatch };
Simulation simulation = Simulation::Aus;
uint32_t spielEndeMs = 0;

// Correcting something that is still being generated does not work: the next
// rally scores over it before anybody can look. So a correction pauses, the way
// a debugger does — the run is held, not thrown away, and carrying on is a
// deliberate press.
void pausieren() {
  if (simulation == Simulation::Aus || mock::paused()) return;
  mock::setPaused(true);
  spielEndeMs = 0;
}

void weiter() {
  if (simulation == Simulation::Aus) return;
  mock::setPaused(false);
}

int spielerAn(char seite) { return seiteZuSpieler[seite == 'A' ? 0 : 1]; }

// The name to show for a half, or the half's own letter when nobody was named.
// Without names everything keeps saying A and B, which is the point of optional.
String nameFuer(char seite) {
  const String &n = spieler[spielerAn(seite)];
  return n.length() ? n : String(seite);
}

bool habenNamen() { return spieler[0].length() || spieler[1].length(); }

char  ersterAufschlag = 'A';
char  aufschlag = 'A';
// Whether the next rally is the serve: from a new game and after every change
// of score, until a rally ends with no point. A lone bounce after that is the
// ball being picked up, not a serve that failed (#83).
bool  aufschlagRally = true;
bool  vorbei = false;
char  sieger = ' ';

String   rally = "";          // z.B. "ABAB"
uint32_t letzterTreffer = 0;

Eintrag  log_[8];
int      logAnzahl = 0;

struct Snapshot { int a, b; char erst, auf; bool ende; char sieg; int logN; };
Snapshot verlauf[16];
int verlaufN = 0;

void sichern() {
  if (verlaufN >= 16) {
    for (int i = 1; i < 16; i++) verlauf[i-1] = verlauf[i];
    verlaufN = 15;
  }
  verlauf[verlaufN++] = { punkteA, punkteB, ersterAufschlag, aufschlag, vorbei, sieger, logAnzahl };
}

// The thresholds are what the bring-up is for, and they were lost on every
// restart: /cfg only moved the variable and logged the change, and the second
// argument was a label for that log, not a key. Anyone who set them at the table
// found 300 again after the next flash.
const char *const CFG_NS = "zwcfg";

void cfgLaden() {
  Preferences p;
  if (!p.begin(CFG_NS, true)) return;
  auto lies = [&p](const char *key, volatile int &ziel) {
    if (p.isKey(key)) ziel = p.getInt(key, ziel);
  };
  lies("thr_a",  schwelleA);
  lies("thr_b",  schwelleB);
  lies("to",     rallyTimeout);
  lies("log_a",  logSchwelleA);
  lies("log_b",  logSchwelleB);
  lies("ratio",  clearRatioPromille);
  lies("btn_l",  tasteLangMs);
  lies("btn_v",  tasteSehrLangMs);
  p.end();
}

void cfgSichern(const char *key, int wert) {
  Preferences p;
  if (!p.begin(CFG_NS, false)) return;
  p.putInt(key, wert);
  p.end();
}

void logEintragen(String folge, String urteil, String hinweis) {
  if (logAnzahl >= 8) {
    for (int i = 1; i < 8; i++) log_[i-1] = log_[i];
    logAnzahl = 7;
  }
  log_[logAnzahl++] = { folge, urteil, hinweis };
}

void punktGeben(char gewinner, String folge, String hinweis, const char *grund,
                const String &marke = "", const String &kommentar = "") {
  const int vorA = punkteA, vorB = punkteB;
  const char vorAufschlag = aufschlag;

  if (gewinner == 'A') punkteA++; else punkteB++;
  aufschlagRally = true;
  if (game::beendet(punkteA, punkteB)) { vorbei = true; sieger = gewinner; }
  else aufschlag = game::aufschlagFuer(punkteA, punkteB, ersterAufschlag);
  logEintragen(folge, String("Punkt fuer ") + nameFuer(gewinner), hinweis);

  // side, player and the name it resolved to at that moment, all three. A
  // mapping that turns out wrong is then correctable in the export instead of
  // invalidating the session.
  diag::point(++pointId, rallyId, grund, hinweis, gewinner,
              vorA, vorB, vorAufschlag, punkteA, punkteB, aufschlag, vorbei,
              marke, kommentar,
              spielerAn(gewinner) == 0 ? "a" : "b", nameFuer(gewinner));
}

void rallyBeenden() {
  if (rally.length() == 0) return;
  String folge = rally;
  rally = "";

  // What the serve rules of #83 say, logged next to what the board decides.
  // rallyBewerten awards serve faults by its own rules since #89; this keeps
  // the guarded version on record to compare against.
  const game::Urteil fehler =
      game::aufschlagPruefen(std::string(folge.c_str()), aufschlag, aufschlagRally);
  diag::rallyEnd(rallyId, folge, "timeout", aufschlag, aufschlagRally, fehler.gewinner);
  rallyOffen = false;

  game::Urteil u = game::rallyBewerten(std::string(folge.c_str()), aufschlag);
  if (u.gewinner != 'A' && u.gewinner != 'B') {
    // No point, on purpose. It still belongs in both logs: the one on the phone,
    // so nobody wonders why the score did not move, and the diagnostic one, so
    // the rally can be counted later against what actually happened.
    logEintragen(folge, "Kein Punkt", String(u.hinweis.c_str()));
    aufschlagRally = false;
    diag::mark(String(u.grund), String(u.hinweis.c_str()), rallyId);
    // A rally that awarded nothing still goes to zaehlwerk, as delta 0 (#17).
    // Does nothing without a zaehlwerk address.
    reporter::rally(' ', folge, 0, rallyId, mock::on());
    return;
  }
  punktGeben(u.gewinner, folge, String(u.hinweis.c_str()), u.grund);
  reporter::rally(u.gewinner, folge, pointId, rallyId, mock::on());
}

// Taking back means taking back a point. Nothing else is what somebody reaching
// for this button wants.
//
// A rally in progress has a snapshot of its own, taken when it started — which
// is the score as it stands right now. Popping only that changed no number and
// read as a button that did nothing, which is exactly how it felt while a
// simulation was running. So an unfinished rally is discarded first and the
// point behind it is taken back after.
// Returns what it did, because a button that silently does the right thing reads
// as a button that does nothing — which is how taking back a double award felt:
// the first press discarded the rally and popped the point in one go, and showed
// neither.
const char *zurueck(const String &marke = "", const String &kommentar = "") {
  bool ballwechselVerworfen = false;
  if (rally.length() > 0) {
    if (verlaufN > 0) verlaufN--;
    diag::rallyEnd(rallyId, rally, "discarded");
    rally = "";
    rallyOffen = false;
    ballwechselVerworfen = true;
  }
  if (verlaufN == 0) return ballwechselVerworfen ? "rally" : "nothing";
  const int vorA = punkteA, vorB = punkteB;
  const char vorAufschlag = aufschlag;
  Snapshot s = verlauf[--verlaufN];
  punkteA = s.a; punkteB = s.b; ersterAufschlag = s.erst;
  aufschlag = s.auf; vorbei = s.ende; sieger = s.sieg;
  logAnzahl = s.logN;
  rally = "";
  aufschlagRally = true;
  diag::point(++pointId, rallyId, "undo", "", ' ',
              vorA, vorB, vorAufschlag, punkteA, punkteB, aufschlag, vorbei,
              marke, kommentar, "", "");
  return ballwechselVerworfen ? "rally+point" : "point";
}

// Whether play is going on, as far as this board can tell: a rally open, or a
// game between its first point and its last. A fob is then refused, so the next
// press stays a point instead of being taken as a check-in (ADR-0007). Whether
// a match is running at zaehlwerk is zaehlwerk's to say, and it does, with a
// 409 — this only keeps the board from eating a point while it asks.
bool spielLaeuft() {
  return rally.length() > 0 || (!vorbei && (punkteA > 0 || punkteB > 0));
}
void neuesSpiel() {
  punkteA = punkteB = 0; vorbei = false; sieger = ' ';
  aufschlag = ersterAufschlag; rally = ""; logAnzahl = 0; verlaufN = 0;
  aufschlagRally = true;
  satzNummer++;
  diag::match("start", spieler[0], spieler[1],
              seiteZuSpieler[0], seiteZuSpieler[1], satzNummer);
}

buttons::Timing tastenZeiten() {
  buttons::Timing t;
  t.longMs = (uint32_t)tasteLangMs;
  t.veryLongMs = (uint32_t)tasteSehrLangMs;
  return t;
}

struct TastenErgebnis {
  buttons::Decision d;
  const char *undone;  // what zurueck() took back, for the page; "" otherwise
};

// The one consumer of every button press (#56). The page's buttons, the radio
// buttons and anything wired later all end here, and nothing below this line
// knows which of them it was — the log does, from the event.
//
// tag and note come only from the page's correction card; a button in the hand
// has neither.
TastenErgebnis taste(const buttons::Event &e, const String &marke = "",
                     const String &kommentar = "") {
  const buttons::Board brett{vorbei, reporter::linked(mock::on())};
  const buttons::Decision d = buttons::decide(e, brett);
  const char *undone = "";
  uint32_t bezug = 0;

  switch (d.action) {
    case buttons::Action::Point:
      pausieren();
      sichern(); rally = "";
      punktGeben(e.side, "", "Manuell vergeben.", "manual", marke, kommentar);
      reporter::manualPoint(e.side, pointId, mock::on());
      bezug = pointId;
      break;
    case buttons::Action::Undo: {
      pausieren();
      const uint32_t vorher = pointId;
      undone = zurueck(marke, kommentar);
      // Joined, one press takes back one point at zaehlwerk, whatever the board
      // had left to pop locally: the score there is the one that counts. An
      // undo event with its own id, never POST /matches/{id}/undo (#67).
      reporter::undo(pointId != vorher ? pointId : 0, mock::on());
      if (pointId != vorher) bezug = pointId;
      break;
    }
    case buttons::Action::StartGame:
    case buttons::Action::NewGame:
      neuesSpiel();
      break;
    case buttons::Action::Refused:
      break;
  }

  diag::button(buttons::sourceName(e.source), buttons::gestureName(e.gesture), e.side,
               e.heldMs, buttons::actionName(d.action), d.reason, bezug, rallyId);
  return {d, undone};
}

buttons::Event virtuell(char seite, buttons::Gesture g, uint32_t ms = 0) {
  return {seite, g, buttons::Source::Virtual, ms};
}


// The one manual step this design cannot remove: a change of ends nobody
// presses silently swaps every point after it. So it is a control on the main
// screen rather than a setting, and it is logged.
void seitenWechseln() {
  const int h = seiteZuSpieler[0];
  seiteZuSpieler[0] = seiteZuSpieler[1];
  seiteZuSpieler[1] = h;
  diag::match("ends_swapped", spieler[0], spieler[1],
              seiteZuSpieler[0], seiteZuSpieler[1], satzNummer);
}

// A short list of who has played here lately, so a name is usually one tap
// rather than typed on a phone at a table.
const char *NVS_ZW = "zaehlwerk";
const char *KEY_NAMEN = "namen";

String letzteNamen() {
  Preferences p;
  p.begin(NVS_ZW, true);
  String n = p.isKey(KEY_NAMEN) ? p.getString(KEY_NAMEN, "") : String();
  p.end();
  return n;
}

void namenMerken(const String &name) {
  if (!name.length()) return;
  String liste = letzteNamen();
  String neu = name;
  // Most recent first, no repeats, and a cap: a list nobody can scan is a list
  // nobody uses.
  int von = 0;
  int wie_viele = 1;
  while (von < (int)liste.length() && wie_viele < 8) {
    int bis = liste.indexOf('\n', von);
    if (bis < 0) bis = liste.length();
    String teil = liste.substring(von, bis);
    if (teil.length() && teil != name) { neu += "\n" + teil; wie_viele++; }
    von = bis + 1;
  }
  Preferences p;
  p.begin(NVS_ZW, false);
  p.putString(KEY_NAMEN, neu);
  p.end();
}

void namenSetzen(const String &a, const String &b) {
  spieler[spielerAn('A')] = a;
  spieler[spielerAn('B')] = b;
  namenMerken(a);
  namenMerken(b);
  diag::match("players", spieler[0], spieler[1],
              seiteZuSpieler[0], seiteZuSpieler[1], satzNummer);
}

/* ================= zaehlwerk-api ================= */
// Once the board has joined a match at zaehlwerk, zaehlwerk owns the score
// (ADR-0008). The board still counts every rally itself, at once, so the page
// does not wait for the network; this is where zaehlwerk's answer replaces
// what it counted. lib/report only hands an answer over once it contains every
// event the board has queued, so nothing the board just counted is taken back
// here and given back a moment later.
//
// Never called without a zaehlwerk address.
void zwUebernehmen(const report::State &s) {
  const report::Board b = report::boardFrom(s, aufschlag);
  const int vorA = punkteA, vorB = punkteB, vorSatz = satzNummer;
  const char vorAufschlag = aufschlag;
  const String vorNamen[2] = {spieler[0], spieler[1]};
  const int vorSeite = seiteZuSpieler[0];

  spieler[0] = s.players[0].c_str();
  spieler[1] = s.players[1].c_str();
  seiteZuSpieler[0] = b.sideToPlayer[0];
  seiteZuSpieler[1] = b.sideToPlayer[1];
  // The log resolves every later point through the last `match` event, so a
  // change of names or ends that came from zaehlwerk is said there too.
  if (vorSeite != seiteZuSpieler[0] || vorNamen[0] != spieler[0] || vorNamen[1] != spieler[1])
    diag::match(vorSeite != seiteZuSpieler[0] ? "ends_swapped" : "players", spieler[0], spieler[1],
                seiteZuSpieler[0], seiteZuSpieler[1], b.setNumber);
  punkteA = b.pointsA;
  punkteB = b.pointsB;
  aufschlag = b.serve;
  ersterAufschlag = b.firstServe;
  vorbei = b.over;
  sieger = b.winner;
  saetze[0] = b.sets[0];
  saetze[1] = b.sets[1];
  satzNummer = b.setNumber;

  // Said only when it differs from what the board had counted: an undo on the
  // phone, a won set, a point the board did not hear, or the first answer
  // after joining. Agreement is the normal case and says nothing.
  if (vorA == punkteA && vorB == punkteB && vorAufschlag == aufschlag && vorSatz == satzNummer)
    return;
  aufschlagRally = true;
  diag::link("adopted", "", s.matchId.c_str(), 0, 0,
             String(",\"from\":{\"a\":") + vorA + ",\"b\":" + vorB + ",\"serve\":\"" +
                 vorAufschlag + "\",\"set_number\":" + vorSatz + "},\"to\":{\"a\":" + punkteA +
                 ",\"b\":" + punkteB + ",\"serve\":\"" + aufschlag + "\",\"set_number\":" +
                 satzNummer + ",\"over\":" + (vorbei ? "true" : "false") + "}");
  logEintragen("", String("Stand vom Zaehlwerk: ") + punkteA + ":" + punkteB, "");
}

/* ================= web server ================= */
WebServer server(80);

// Every virtual press answers the same way, so the page can say what happened —
// a refused press that answers "ok" reads as a button that is broken.
void tasteAntworten(const TastenErgebnis &r) {
  String j = String("{\"action\":\"") + buttons::actionName(r.d.action) + "\"";
  if (*r.d.reason) j += String(",\"reason\":\"") + r.d.reason + "\"";
  if (*r.undone) j += String(",\"undone\":\"") + r.undone + "\"";
  j += ",\"a\":" + String(punkteA) + ",\"b\":" + String(punkteB) + "}";
  server.send(200, "application/json", j);
}

bool webserverLaeuft = false;

// The page lives in data/index.html and is uploaded to LittleFS separately from
// the firmware. That is what makes it updatable on its own — and what makes the
// fallback below necessary: `pio run -t upload` writes the app and not the
// filesystem, so a board flashed without one would have no page at all and no
// way back in except a cable.
const char NOTFALL[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zählwerk — Dateisystem fehlt</title><style>
body{background:#EEF1F4;color:#1B2430;font:15px/1.6 system-ui,sans-serif;padding:20px}
.w{max-width:520px;margin:0 auto}
.c{background:#fff;border:1px solid #C4CDD6;border-radius:10px;padding:16px;margin-top:14px}
h1{font-size:17px;margin-bottom:6px}
code{font:12px ui-monospace,monospace;background:#EEF1F4;padding:1px 5px;border-radius:4px}
input,button{width:100%;padding:9px;margin-top:8px;border:1px solid #C4CDD6;
border-radius:6px;font:14px inherit;background:#fff}
button{font-weight:600;cursor:pointer}
.m{font-size:12px;color:#5C6B7A;min-height:16px;margin-top:8px}
</style></head><body><div class="w">
<h1>Die Seite fehlt</h1>
<p>Die Firmware laeuft, aber <code>/index.html</code> liegt nicht im Dateisystem.
Das passiert nach einem <code>upload</code> ohne <code>uploadfs</code>.</p>
<div class="c">
  <p>Von hier aus einspielen, ohne Kabel:</p>
  <select id="t">
    <option value="/updatefs">Dateisystem (littlefs.bin)</option>
    <option value="/update">Firmware (firmware.bin)</option>
  </select>
  <input type="file" id="f" accept=".bin">
  <input type="password" id="p" placeholder="OTA-Passwort" autocomplete="off">
  <button onclick="s()">Einspielen</button>
  <div class="m" id="m"></div>
</div>
<div class="c"><p>Oder am Rechner: <code>task ota:fs</code></p></div>
</div><script>
function s(){
  if(!f.files.length){m.textContent='Erst eine .bin waehlen.';return}
  if(!p.value){m.textContent='Ohne Passwort geht es nicht.';return}
  const d=new FormData();d.append('f',f.files[0]);
  const x=new XMLHttpRequest();
  x.open('POST',t.value,true);
  x.setRequestHeader('Authorization','Basic '+btoa('zaehlwerk:'+p.value));
  m.textContent='Laedt hoch ...';
  x.onload=()=>{m.textContent=x.status===200
    ?'Eingespielt. Neustart, die Seite kommt in ein paar Sekunden.'
    :(x.status===401?'Passwort falsch.':'Fehlgeschlagen: '+x.responseText);
    if(x.status===200)setTimeout(()=>location.reload(),8000)};
  x.onerror=()=>{m.textContent='Verbindung abgebrochen.'};
  x.send(d);
}
</script></body></html>)HTML";

String jsonEscape(String s) { s.replace("\"", "'"); return s; }

void handleState() {
  String j = "{";
  j += "\"a\":" + String(punkteA) + ",\"b\":" + String(punkteB);
  j += ",\"serve\":\"" + String(aufschlag) + "\"";
  j += ",\"over\":" + String(vorbei ? "true" : "false");
  j += ",\"winner\":\"" + String(sieger) + "\"";
  j += ",\"rally\":\"" + rally + "\"";
  j += ",\"ta\":" + String(schwelleA) + ",\"tb\":" + String(schwelleB);
  // The log thresholds decide what reaches the sink at all, so they belong in
  // the UI and not only behind a curl. Step 1 of the bring-up is setting them
  // per channel against each channel's own floor, which happens at the table.
  j += ",\"la\":" + String(logSchwelleA) + ",\"lb\":" + String(logSchwelleB);
  j += ",\"cr\":" + String(clearRatioPromille);
  j += ",\"to\":" + String(rallyTimeout);
  j += ",\"bl\":" + String(tasteLangMs) + ",\"bv\":" + String(tasteSehrLangMs);
  j += ",\"linked\":" + String(reporter::linked(mock::on()) ? "true" : "false");
  j += ",\"na\":\"" + jsonEscape(nameFuer('A')) + "\"";
  j += ",\"nb\":\"" + jsonEscape(nameFuer('B')) + "\"";
  j += ",\"named\":" + String(habenNamen() ? "true" : "false");
  j += ",\"satz\":" + String(satzNummer);
  j += ",\"mock\":" + String(mock::on() ? "true" : "false");
  j += ",\"auto\":" + String(mock::autoplay() ? "true" : "false");
  j += ",\"sim\":" + String((int)simulation);
  j += ",\"pause\":" + String(mock::paused() ? "true" : "false");
  j += ",\"sa\":" + String(saetze[spielerAn('A')]);
  j += ",\"sb\":" + String(saetze[spielerAn('B')]);
  j += ",\"rec\":" + String(diag::sinkAlive() ? "true" : "false");
  j += ",\"img\":\"" + String(ota::imageState()) + "\"";
  j += ",\"up\":" + String(ota::progress());
  j += ",\"log\":[";
  for (int i = 0; i < logAnzahl; i++) {
    if (i) j += ",";
    j += "{\"f\":\"" + log_[i].folge + "\",\"u\":\"" + jsonEscape(log_[i].urteil)
       + "\",\"h\":\"" + jsonEscape(log_[i].hinweis) + "\"}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleVersion() {
  String j = String("{\"fw\":\"") + ZW_FW_VERSION
           + "\",\"git\":\"" + ZW_GIT_HASH
           + "\",\"built\":\"" + ZW_BUILD_DATE + "\""
           + ",\"repo\":\"" + ZW_GIT_REPO + "\""
           + ",\"uptime_s\":" + String((uint32_t)(millis() / 1000))
           + ",\"dirty\":" + (ZW_GIT_DIRTY ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

// An update writes into the second app slot. While that happens no task should
// be wanting flash and CPU, and the running game belongs ended rather than cut
// off mid-rally. The session end as a log event comes with #15; for now it is
// the serial line.
void samplingAnhalten() {
  if (sensorTaskHandle) vTaskSuspend(sensorTaskHandle);
  if (rally.length() > 0) rallyBeenden();
  xQueueReset(queue);
  Serial.println("[ota] sampling suspended, session ended");
}

void samplingFortsetzen() {
  if (sensorTaskHandle) vTaskResume(sensorTaskHandle);
  Serial.println("[ota] sampling resumed");
}

// The whole parameter set, the way the session event carries it. Everything
// downstream is read against this, so a session with no parameters is a session
// that cannot be compared with another.
String parameterJson() {
  String j = "{";
  j += "\"threshold_a\":" + String(schwelleA) + ",\"threshold_b\":" + String(schwelleB);
  j += ",\"log_threshold_a\":" + String(logSchwelleA) +
       ",\"log_threshold_b\":" + String(logSchwelleB);
  j += ",\"clear_ratio\":" + String(clearRatioPromille / 1000.0, 3);
  j += ",\"peak_window_us\":" + String((long)FENSTER_MS * 1000);
  j += ",\"deadtime_us\":" + String((long)SPERRE_MS * 1000);
  j += ",\"rally_timeout_ms\":" + String(rallyTimeout);
  j += ",\"pre_trigger_samples\":" + String((int)diag::PRE_SAMPLES);
  j += ",\"capture_samples\":" + String((int)diag::CAPTURE_SAMPLES);
  j += ",\"button_long_ms\":" + String(tasteLangMs);
  j += ",\"button_very_long_ms\":" + String(tasteSehrLangMs);
  j += "}";
  return j;
}

// What the page needs to say which network the board is on — and the channel,
// because ESP-NOW peers have to sit on it.
void handleNet() {
  String j = "{";
  j += "\"mode\":\"" + String(net::modeName()) + "\"";
  j += ",\"ssid\":\"" + jsonEscape(net::ssid()) + "\"";
  j += ",\"ip\":\"" + net::ip().toString() + "\"";
  j += ",\"ch\":" + String(net::channel());
  j += ",\"host\":\"" + net::hostname() + ".local\"";
  j += ",\"to\":" + String(net::staTimeout() / 1000);
  j += "}";
  server.send(200, "application/json", j);
}

// POST rather than GET: a password does not belong in a URL, where it would sit
// in the browser history. Stored and applied on the next start — switching the
// radio mid-request would drop the answer, and a wrong password would leave no
// way back in. After a restart a wrong one lands in the access point.
void handleWifi() {
  if (server.hasArg("to")) {
    long s = server.arg("to").toInt();
    if (s >= 5 && s <= 120) net::setStaTimeout((uint32_t)s * 1000);
  }
  if (server.hasArg("ssid")) {
    net::setCredentials(server.arg("ssid"), server.arg("pass"));
    server.send(200, "text/plain", "Gespeichert. Neustart — der neue Stand gilt danach.\n");
    delay(200);
    ESP.restart();
    return;
  }
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);
  Serial.printf("\nZaehlwerk %s  git %s  built %s\n%s\n",
                ZW_FW_VERSION, ZW_GIT_HASH, ZW_BUILD_DATE, ZW_GIT_REPO);

  // Before anything reads them: what was set at the table outlives the reboot.
  cfgLaden();

  // The page comes from here. A board without it still answers, with a page
  // that can put one back — see NOTFALL above.
  if (!LittleFS.begin(false)) {
    Serial.println("[fs] no filesystem — serving the recovery page only");
  } else {
    File f = LittleFS.open("/index.html", "r");
    Serial.printf("[fs] index.html: %s\n", f ? String(f.size()).c_str() : "missing");
    if (f) f.close();
  }
  Serial.println("Start Game");

#ifdef DEFAULT_MOCK
  mock::begin(true);
#else
  mock::begin(false);
#endif

  queue = xQueueCreate(16, sizeof(Treffer));
  xTaskCreatePinnedToCore(sensorTask, "sensor", 8192, NULL, 3, &sensorTaskHandle,
                          ZW_SENSOR_CORE);

  // Station on the configured network, our own access point if that does not
  // come up in time. Blocks for up to the timeout — deliberately, see net.cpp.
  net::begin({ ZW_HOSTNAME, AP_PREFIX, AP_PASS,
               ZW_STA_SSID, ZW_STA_PASS, ZW_STA_TIMEOUT_MS });

  // The link to zaehlwerk-api, on the core that does not sample. Without an
  // address it starts nothing (ADR-0008).
  reporter::begin();
  // The fob reader, on the core that does not sample (ADR-0003, ADR-0007).
  // After the reporter: that is where ingest::begin() reads the address and
  // the counter both of them share.
  fob::begin();

  server.on("/", []{
    File f = LittleFS.open("/index.html", "r");
    if (!f || f.isDirectory()) {
      // 503 rather than 200: something is wrong and a monitor should say so,
      // even though the page it returns is useful.
      server.send_P(503, "text/html", NOTFALL);
      return;
    }
    // Without this a phone keeps serving the page it cached, and a freshly
    // uploaded UI never arrives — which looks exactly like a change that was
    // never made. The page is 41 KB over the local network, most of it the
    // mascot; there is nothing to save here.
    server.sendHeader("Cache-Control", "no-store");
    server.streamFile(f, "text/html");
    f.close();
  });
  server.on("/state", handleState);
  server.on("/version", handleVersion);
  // The link to zaehlwerk-api (#66): its state, and the address set or cleared
  // on the page. POST, like /wifi. The address is no secret and is shown back,
  // because seeing which zaehlwerk the board talks to is what makes a wrong one
  // diagnosable.
  server.on("/zaehlwerk", HTTP_GET, []{
    server.send(200, "application/json", reporter::statusJson(mock::on()));
  });
  server.on("/zaehlwerk/url", HTTP_POST, []{
    if (!server.hasArg("url")) {
      server.send(400, "text/plain", "url fehlt\n");
      return;
    }
    String msg;
    const bool ok = reporter::setUrl(server.arg("url"), msg);
    server.send(ok ? 200 : 400, "text/plain", msg + "\n");
  });
  // Check-in by fob. ?press=A or ?press=B stands in for a paired button until the
  // ESP-NOW path of #58 exists, and does only that: with no fob waiting it is not
  // a point, just a press that came to nothing.
  server.on("/checkin", []{
    if (server.hasArg("press")) {
      const String s = server.arg("press");
      const bool taken = (s == "A" || s == "B") && fob::press(s[0]);
      server.send(200, "application/json",
                  String("{\"taken\":") + (taken ? "true" : "false") +
                  ",\"checkin\":" + fob::stateJson() + "}");
      return;
    }
    server.send(200, "application/json", fob::stateJson());
  });
  // zaehlwerk's CHECKIN_TOKEN (#65). POST, like /wifi: a token does not belong
  // in a URL. `token=` empty clears it. The answer says whether one is set and
  // never echoes it, and neither does the log line.
  server.on("/checkin/token", HTTP_POST, []{
    if (!server.hasArg("token")) {
      server.send(400, "text/plain", "token fehlt\n");
      return;
    }
    String t = server.arg("token");
    const ingest::TokenChange c = ingest::setCheckinToken(t);
    if (t.length()) memset(&t[0], 0, t.length());
    if (c == ingest::TokenChange::Refused) {
      server.send(400, "text/plain",
                  "Nicht gespeichert: 1 bis 256 Zeichen, ohne Leerzeichen und Umlaute.\n");
      return;
    }
    Serial.println(c == ingest::TokenChange::Set ? "[checkin] token set on the page"
                                                 : "[checkin] token cleared on the page");
    server.send(200, "application/json", fob::stateJson());
  });
  server.on("/net", handleNet);
  server.on("/wifi", HTTP_POST, handleWifi);
  // The page's buttons are virtual buttons (#56): each is a press with a fixed
  // gesture through the same consumer as a button in the hand, so nothing that
  // already calls these routes breaks and nothing behind them is different.
  server.on("/punkt", []{
    const char s = server.arg("s") == "B" ? 'B' : 'A';
    tasteAntworten(taste(virtuell(s, buttons::Gesture::Short),
                         server.arg("tag"), server.arg("note")));
  });
  server.on("/zurueck", []{
    tasteAntworten(taste(virtuell(' ', buttons::Gesture::Long),
                         server.arg("tag"), server.arg("note")));
  });
  server.on("/neu", []{
    tasteAntworten(taste(virtuell(' ', buttons::Gesture::VeryLong)));
  });
  // A held virtual button: the page measures how long it was held and the
  // board decides what that was, with the same thresholds a button would use.
  server.on("/taste", []{
    const String s = server.arg("s");
    const char seite = s == "A" ? 'A' : s == "B" ? 'B' : ' ';
    const uint32_t ms = (uint32_t)server.arg("ms").toInt();
    tasteAntworten(taste(virtuell(seite, buttons::classify(ms, tastenZeiten()), ms),
                         server.arg("tag"), server.arg("note")));
  });
  server.on("/cfg", []{
    auto setzen = [](const char *arg, const char *name, const char *key,
                     volatile int &ziel) {
      if (!server.hasArg(arg)) return;
      const int neu = server.arg(arg).toInt();
      if (neu == ziel) return;
      diag::param(name, String(ziel), String(neu), "web");
      ziel = neu;
      cfgSichern(key, neu);
    };
    setzen("a",  "threshold_a",      "thr_a", schwelleA);
    setzen("b",  "threshold_b",      "thr_b", schwelleB);
    setzen("t",  "rally_timeout_ms", "to",    rallyTimeout);
    setzen("la", "log_threshold_a",  "log_a", logSchwelleA);
    setzen("lb", "log_threshold_b",  "log_b", logSchwelleB);
    setzen("cr", "clear_ratio",      "ratio", clearRatioPromille);
    // The two press thresholds only make sense together, so they are checked
    // as a pair and a pair nobody could press is not stored.
    if (server.hasArg("bl") || server.hasArg("bv")) {
      buttons::Timing t = tastenZeiten();
      if (server.hasArg("bl")) t.longMs = (uint32_t)server.arg("bl").toInt();
      if (server.hasArg("bv")) t.veryLongMs = (uint32_t)server.arg("bv").toInt();
      if (!buttons::valid(t)) {
        server.send(400, "text/plain", "long from 200 ms, very long at least 500 ms after it and at most 10 s\n");
        return;
      }
      setzen("bl", "button_long_ms",      "btn_l", tasteLangMs);
      setzen("bv", "button_very_long_ms", "btn_v", tasteSehrLangMs);
    }
    server.send(200, "text/plain", "ok");
  });

  server.on("/spieler", []{
    if (server.hasArg("a") || server.hasArg("b"))
      namenSetzen(server.arg("a"), server.arg("b"));
    String j = String("{\"a\":\"") + jsonEscape(nameFuer('A')) +
               "\",\"b\":\"" + jsonEscape(nameFuer('B')) +
               "\",\"named\":" + (habenNamen() ? "true" : "false") +
               ",\"letzte\":[";
    String l = letzteNamen();
    int von = 0; bool erst = true;
    while (von < (int)l.length()) {
      int bis = l.indexOf('\n', von);
      if (bis < 0) bis = l.length();
      String teil = l.substring(von, bis);
      if (teil.length()) {
        if (!erst) j += ",";
        j += "\"" + jsonEscape(teil) + "\"";
        erst = false;
      }
      von = bis + 1;
    }
    j += "]}";
    server.send(200, "application/json", j);
  });

  // The mock is not a switch tucked away somewhere. A board in mock mode that
  // looks like a board at a table is how a made-up session gets read as a real
  // one — hence the banner, and hence switching it starts a new session rather
  // than continuing the one that was going.
  server.on("/mock", []{
    const String t = server.arg("t");
    if (t == "on" || t == "off") {
      const bool an = (t == "on");
      if (an != mock::on()) {
        samplingAnhalten();
        mock::setOn(an);
        neuesSpiel();
        diag::newSession(an ? "mock" : "adc", "mode_switch");
        samplingFortsetzen();
      }
    } else if (t == "rally") {
      mock::playRally();
    } else if (t == "pause") {
      pausieren();
    } else if (t == "play") {
      weiter();
    } else if (t == "auto" || t == "game" || t == "match") {
      const bool an = server.arg("on") != "0";
      if (!an) {
        simulation = Simulation::Aus;
      } else {
        if (t == "game")  simulation = Simulation::EinSpiel;
        if (t == "match") { simulation = Simulation::EinMatch; saetze[0] = saetze[1] = 0; }
        if (t == "auto")  simulation = Simulation::Dauerhaft;
        if (t != "auto") neuesSpiel();
      }
      mock::setAutoplay(an);
      mock::setPaused(false);
      spielEndeMs = 0;
    } else if (t.length()) {
      const char s = server.arg("s") == "B" ? 'B' : 'A';
      mock::Kind k = mock::Kind::Bounce;
      if (t == "weak")  k = mock::Kind::Weak;
      if (t == "ghost") k = mock::Kind::Ghost;
      if (t == "net")   k = mock::Kind::Net;
      mock::trigger(k, s);
    }
    server.send(200, "application/json",
                String("{\"mock\":") + (mock::on() ? "true" : "false") +
                ",\"auto\":" + (mock::autoplay() ? "true" : "false") +
                ",\"sim\":" + String((int)simulation) +
                ",\"pause\":" + (mock::paused() ? "true" : "false") + "}");
  });

  // Something seen that changed no score. Without this the only way to write a
  // thing down was to correct something, which is a poor reason to touch a
  // scoreboard.
  server.on("/notiz", []{
    const String t = server.arg("note");
    const String m = server.arg("tag");
    if (t.length() || m.length()) {
      diag::mark(m, t, rallyId);
      logEintragen("", "Notiz", t.length() ? t : m);
    }
    server.send(200, "text/plain", "ok");
  });

  server.on("/seiten", []{
    seitenWechseln();
    server.send(200, "text/plain", "ok");
  });
  // Who served first. Unlinked it was always A, with no way to say otherwise:
  // a game begun from B had the board one serve off from the first point on,
  // and taking a point back only restored the wrong server. Since #89 that
  // costs points too — a first bounce on the half the board thinks is the
  // receiver's is a serve fault. Linked, zaehlwerk says who serves.
  server.on("/aufschlag", []{
    if (reporter::linked(mock::on())) {
      server.send(409, "application/json", "{\"reason\":\"serve_on_zaehlwerk\"}");
      return;
    }
    const char vorher = ersterAufschlag;
    ersterAufschlag = game::andere(ersterAufschlag);
    if (!vorbei) aufschlag = game::aufschlagFuer(punkteA, punkteB, ersterAufschlag);
    diag::param("first_serve", String(vorher), String(ersterAufschlag), "web");
    server.send(200, "application/json",
                String("{\"serve\":\"") + aufschlag + "\",\"first\":\"" + ersterAufschlag + "\"}");
  });

  server.on("/diag", []{
    if (server.hasArg("host"))
      diag::setSink(server.arg("host"),
                    server.hasArg("port") ? server.arg("port").toInt() : diag::sinkPort());
    if (server.hasArg("on")) diag::setEnabled(server.arg("on") == "1");
    String j = String("{\"host\":\"") + diag::sinkHost() + "\",\"port\":" +
               diag::sinkPort() + ",\"on\":" + (diag::enabled() ? "true" : "false") +
               ",\"alive\":" + (diag::sinkAlive() ? "true" : "false") +
               ",\"session\":\"" + diag::sessionId() + "\",\"dropped\":" +
               diag::droppedEvents() + ",\"held\":" + diag::heldEvents();
    j += ",\"a\":" + String(letzteA) + ",\"b\":" + String(letzteB);
    j += ",\"base_a\":" + String(baselineA) + ",\"base_b\":" + String(baselineB);
    j += ",\"min_a\":" + String(minA) + ",\"max_a\":" + String(maxA);
    j += ",\"min_b\":" + String(minB) + ",\"max_b\":" + String(maxB);
    j += ",\"ticks\":" + String(sensorTicks) + "}";
    // Reset the window: the next poll reports the next stretch, not all of time.
    minA = 4095; maxA = 0; minB = 4095; maxB = 0;
    server.send(200, "application/json", j);
  });
  server.begin();
  webserverLaeuft = true;

  ota::begin(server, { ZW_HOSTNAME, ZW_OTA_PASS, samplingAnhalten, samplingFortsetzen });

  // Last, so the session event carries a network that is already up and the
  // parameters as they actually stand.
  diag::begin({ ZW_DEVICE_ID, mock::on() ? "mock" : "adc", parameterJson(),
                "boot", parameterJson });
  diag::match("start", spieler[0], spieler[1],
              seiteZuSpieler[0], seiteZuSpieler[1], satzNummer);
}

// The verdict on our own start that the rollback listens to. Deliberately more
// than "setup() returned": wifi is up, the web server is listening, and the
// sensor task has been round its loop often enough to be running rather than
// merely created.
bool startWarSauber() {
  return webserverLaeuft
      && net::ip() != IPAddress((uint32_t)0)
      && sensorTicks > 1000;
}

// A simulated run decides for itself what happens when a game ends. It waits a
// couple of seconds first, so somebody watching the page sees the final score
// rather than a number that jumps.
void simulationTreiben() {
  if (simulation == Simulation::Aus || mock::paused() || !vorbei) {
    spielEndeMs = 0;
    return;
  }
  if (!spielEndeMs) { spielEndeMs = millis(); return; }
  if (millis() - spielEndeMs < 2500) return;
  spielEndeMs = 0;

  if (simulation == Simulation::EinSpiel) {
    simulation = Simulation::Aus;
    mock::setAutoplay(false);
    return;
  }
  if (simulation == Simulation::Dauerhaft) {
    neuesSpiel();
    return;
  }

  // A match. The set goes to the player, not to the half — they change ends
  // between sets, which is the whole reason the mapping exists.
  saetze[spielerAn(sieger)]++;
  if (saetze[0] >= 3 || saetze[1] >= 3) {
    simulation = Simulation::Aus;
    mock::setAutoplay(false);
    diag::match("end", spieler[0], spieler[1],
                seiteZuSpieler[0], seiteZuSpieler[1], satzNummer);
    return;
  }
  seitenWechseln();
  neuesSpiel();
}

void loop() {
  server.handleClient();
  net::tick();
  simulationTreiben();
  mock::tick();
  diag::tick();
  reporter::tick(zwUebernehmen, mock::on());
  fob::setBusy(spielLaeuft());
  fob::tick();
  ota::handle();
  ota::tick(startWarSauber());

  Treffer t;
  while (xQueueReceive(queue, &t, 0) == pdTRUE) {
    if (vorbei) continue;
    if (rally.length() == 0) {
      sichern();
      diag::rallyStart(++rallyId);
      rallyOffen = true;
    }
    rally += t.seite;
    letzterTreffer = t.t;
    Serial.printf("hit %c    A:%d B:%d\n", t.seite, t.spitzeA, t.spitzeB);
  }

  if (rally.length() > 0 && millis() - letzterTreffer > (uint32_t)rallyTimeout)
    rallyBeenden();
}
