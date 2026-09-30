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

#include "config.h"
#include "diag.h"
#include "game.h"
#include "mock.h"
#include "net.h"
#include "ota.h"
#include "version.h"

/* ================= configuration ================= */
const int  PIN_A = ZW_PIN_A;
const int  PIN_B = ZW_PIN_B;
const char *AP_PREFIX = ZW_AP_SSID;   // the chip id is appended, see net.cpp
const char *AP_PASS   = ZW_AP_PASS;

volatile int schwelleA   = 300;
volatile int schwelleB   = 300;
volatile int rallyTimeout = 3000;   // ms Stille = Ballwechsel vorbei

// Everything above log_threshold is written to the log, whether or not it
// counts. Below the counting threshold on purpose: the crossings that were
// thrown away are the half the scoreboard never mentions.
volatile int logSchwelleA = 120;
volatile int logSchwelleB = 120;

// How much louder one channel has to be before the side is more than a guess.
// It classifies today, it does not yet decide — see diag::Hit::counted.
volatile int clearRatioPromille = 1800;

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
#ifndef ZW_SENSOR_CORE
#define ZW_SENSOR_CORE 0
#endif
#if ZW_CONTINUOUS_SAMPLING
  #define ZW_SENSOR_YIELD() taskYIELD()
#else
  #define ZW_SENSOR_YIELD() vTaskDelay(1)
#endif

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
bool rallyOffen = false;

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
      h.rallyId = rallyId;
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

      h.rallyId = rallyId;
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

  diag::rallyEnd(rallyId, folge, "timeout");
  rallyOffen = false;

  game::Urteil u = game::rallyBewerten(std::string(folge.c_str()), aufschlag);
  if (u.gewinner != 'A' && u.gewinner != 'B') {
    // No point, on purpose. It still belongs in both logs: the one on the phone,
    // so nobody wonders why the score did not move, and the diagnostic one, so
    // the rally can be counted later against what actually happened.
    logEintragen(folge, "Kein Punkt", String(u.hinweis.c_str()));
    diag::mark(String(u.grund), String(u.hinweis.c_str()), rallyId);
    return;
  }
  punktGeben(u.gewinner, folge, String(u.hinweis.c_str()), u.grund);
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
  diag::point(++pointId, rallyId, "undo", "", ' ',
              vorA, vorB, vorAufschlag, punkteA, punkteB, aufschlag, vorbei,
              marke, kommentar, "", "");
  return ballwechselVerworfen ? "rally+point" : "point";
}

void neuesSpiel() {
  punkteA = punkteB = 0; vorbei = false; sieger = ' ';
  aufschlag = ersterAufschlag; rally = ""; logAnzahl = 0; verlaufN = 0;
  satzNummer++;
  diag::match("start", spieler[0], spieler[1],
              seiteZuSpieler[0], seiteZuSpieler[1], satzNummer);
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

/* ================= web server ================= */
WebServer server(80);
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

  server.on("/", []{
    File f = LittleFS.open("/index.html", "r");
    if (!f || f.isDirectory()) {
      // 503 rather than 200: something is wrong and a monitor should say so,
      // even though the page it returns is useful.
      server.send_P(503, "text/html", NOTFALL);
      return;
    }
    server.streamFile(f, "text/html");
    f.close();
  });
  server.on("/state", handleState);
  server.on("/version", handleVersion);
  server.on("/net", handleNet);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/punkt", []{
    pausieren();
    sichern(); rally = "";
    char s = server.arg("s") == "B" ? 'B' : 'A';
    punktGeben(s, "", "Manuell vergeben.", "manual",
               server.arg("tag"), server.arg("note"));
    server.send(200, "text/plain", "ok");
  });
  server.on("/zurueck", []{
    pausieren();
    const char *was = zurueck(server.arg("tag"), server.arg("note"));
    server.send(200, "application/json",
                String("{\"undone\":\"") + was + "\",\"a\":" + punkteA +
                ",\"b\":" + punkteB + "}");
  });
  server.on("/neu",     []{ neuesSpiel(); server.send(200, "text/plain", "ok"); });
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
