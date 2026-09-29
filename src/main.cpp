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

  How the firmware is laid out (ADR-0003):
    core 1  sensor task, alone, sampling continuously and never yielding
    core 0  web server, game logic, wifi, logging, OTA
  The split matters, and so do the sides. The wifi and lwIP tasks live on core 0,
  so a sampler there shares a core with the radio: on the bench that cost a
  measured 24 ms gap in the middle of a peak window. Core 1 belongs to the
  sampler; anything else that wants it has to justify itself.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <driver/adc.h>

#include <string>

#include "config.h"
#include "diag.h"
#include "game.h"
#include "net.h"
#include "ota.h"
#include "version.h"

/* ================= configuration ================= */
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

const int SPERRE_MS  = 60;          // Nachklingen
const int FENSTER_MS = 30;          // Vergleichsfenster zwischen den Kanälen

/* ================= sensor task ================= */
struct Treffer { char seite; int spitzeA; int spitzeB; uint32_t t; };
QueueHandle_t queue;

// A handle, to suspend sampling while an update is written, and a counter that
// lets anything outside see the task is really running. The rollback needs
// both: with no sign of life from the sensor task, an image that boots but can
// no longer measure would count as "started cleanly".
TaskHandle_t sensorTaskHandle = nullptr;
volatile uint32_t sensorTicks = 0;

// The quiet level of each channel, tracked while nothing is happening. A peak
// means little without it — the channels sit at different levels and drift.
volatile int baselineA = 0, baselineB = 0;

// Ids. Every hit belongs to a rally, every point to the rally it ended, so a
// correction later can point at one thing rather than at a span of time.
uint32_t rallyId = 0, pointId = 0;
bool rallyOffen = false;

// The pre-trigger ring: what the sampler managed to take before the crossing.
// At one sample per millisecond that is not a curve yet; #10 is what makes it
// one. The shape is right either way, so the sink does not change with it.
diag::Sample vorlauf[diag::PRE_SAMPLES];
size_t vorlaufKopf = 0;
uint32_t vorlaufFuell = 0;

// GPIO34 and GPIO35 are ADC1 channels 6 and 7. ADC1 is not a preference: ADC2
// is unavailable while wifi is running.
//
// The mapping is fixed by the chip, so moving a piezo to another pin has to
// move the channel with it. The assertion turns that into a build error rather
// than a board that reads the wrong pin and says nothing.
static_assert(ZW_PIN_A == 34 && ZW_PIN_B == 35,
              "the ADC1 channels below are chosen for GPIO34 and GPIO35");
const adc1_channel_t KANAL_A = ADC1_CHANNEL_6;
const adc1_channel_t KANAL_B = ADC1_CHANNEL_7;

void sensorTask(void *) {
  // The same configuration analogRead() would apply, applied once instead of on
  // every call. That is where the time went: 84 microseconds per read, measured,
  // against a bounce that rises in tens.
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(KANAL_A, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(KANAL_B, ADC_ATTEN_DB_11);

  // This core is the sampler's. Nothing else runs here, so starving the idle
  // task is deliberate rather than an oversight — and the watchdog has to be
  // told, or it reports the design as a fault.
  disableCore1WDT();

  uint32_t sperreBis = 0;

  for (;;) {
    sensorTicks++;

    const uint32_t jetztUs = micros();
    int a = adc1_get_raw(KANAL_A);
    int b = adc1_get_raw(KANAL_B);

    vorlauf[vorlaufKopf] = { (uint16_t)(jetztUs & 0xffff), (int16_t)a, (int16_t)b };
    vorlaufKopf = (vorlaufKopf + 1) % diag::PRE_SAMPLES;
    vorlaufFuell++;

    const bool sperre = millis() < sperreBis;
    const bool uebertritt = (a >= logSchwelleA || b >= logSchwelleB);

    if (!uebertritt) {
      // Quiet: let the baselines follow, slowly enough that a hit does not
      // drag them along.
      // Slow enough that a hit does not drag them along. No delay here: the
      // loop runs flat out, which is the point of the change.
      baselineA += (a - baselineA) / 256;
      baselineB += (b - baselineB) / 256;
      continue;
    }

    const bool zaehlt = !sperre && (a >= schwelleA || b >= schwelleB);

    if (!zaehlt) {
      // Logged, but nothing else changes. No peak window here on purpose: it
      // would cost thirty milliseconds of blindness that the old code did not
      // spend, and this change is meant to measure detection, not alter it.
      diag::Hit h{};
      h.rallyId = rallyId;
      h.side = ' ';
      h.decision = sperre ? diag::Decision::Deadtime : diag::Decision::BelowThreshold;
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
        h.samples[n] = vorlauf[k];
        h.samples[n].dtUs = 0;  // filled in below, once t0 is known
        n++;
      }
      const uint16_t vorlaufAnzahl = n;

      uint32_t start = millis();
      int spA = a, spB = b;
      int32_t kreuzA = a >= schwelleA ? 0 : -1;
      int32_t kreuzB = b >= schwelleB ? 0 : -1;
      while (millis() - start < (uint32_t)FENSTER_MS) {
        uint32_t tUs = micros();
        int va = adc1_get_raw(KANAL_A); if (va > spA) spA = va;
        int vb = adc1_get_raw(KANAL_B); if (vb > spB) spB = vb;
        if (kreuzA < 0 && va >= schwelleA) kreuzA = (int32_t)(tUs - jetztUs);
        if (kreuzB < 0 && vb >= schwelleB) kreuzB = (int32_t)(tUs - jetztUs);
        if (n < diag::CAPTURE_SAMPLES) {
          h.samples[n++] = { (uint16_t)(tUs - jetztUs), (int16_t)va, (int16_t)vb };
        }
      }
      for (uint16_t i = 0; i < vorlaufAnzahl; i++) h.samples[i].dtUs = 0;

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
  }
}

/* ================= game state ================= */
struct Eintrag { String folge; String urteil; String hinweis; };

int   punkteA = 0, punkteB = 0;
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

void logEintragen(String folge, String urteil, String hinweis) {
  if (logAnzahl >= 8) {
    for (int i = 1; i < 8; i++) log_[i-1] = log_[i];
    logAnzahl = 7;
  }
  log_[logAnzahl++] = { folge, urteil, hinweis };
}

void punktGeben(char gewinner, String folge, String hinweis, const char *grund) {
  const int vorA = punkteA, vorB = punkteB;
  const char vorAufschlag = aufschlag;

  if (gewinner == 'A') punkteA++; else punkteB++;
  if (game::beendet(punkteA, punkteB)) { vorbei = true; sieger = gewinner; }
  else aufschlag = game::aufschlagFuer(punkteA, punkteB, ersterAufschlag);
  logEintragen(folge, String("Punkt fuer ") + gewinner, hinweis);

  diag::point(++pointId, rallyId, grund, hinweis, gewinner,
              vorA, vorB, vorAufschlag, punkteA, punkteB, aufschlag, vorbei);
}

void rallyBeenden() {
  if (rally.length() == 0) return;
  String folge = rally;
  rally = "";

  diag::rallyEnd(rallyId, folge, "timeout");
  rallyOffen = false;

  game::Urteil u = game::rallyBewerten(std::string(folge.c_str()), aufschlag);
  punktGeben(u.gewinner, folge, String(u.hinweis.c_str()), u.grund);
}

void zurueck() {
  if (verlaufN == 0) return;
  const int vorA = punkteA, vorB = punkteB;
  const char vorAufschlag = aufschlag;
  Snapshot s = verlauf[--verlaufN];
  punkteA = s.a; punkteB = s.b; ersterAufschlag = s.erst;
  aufschlag = s.auf; vorbei = s.ende; sieger = s.sieg;
  logAnzahl = s.logN;
  rally = "";
  diag::point(++pointId, rallyId, "undo", "", ' ',
              vorA, vorB, vorAufschlag, punkteA, punkteB, aufschlag, vorbei);
}

void neuesSpiel() {
  punkteA = punkteB = 0; vorbei = false; sieger = ' ';
  aufschlag = ersterAufschlag; rally = ""; logAnzahl = 0; verlaufN = 0;
}

/* ================= web server ================= */
WebServer server(80);
bool webserverLaeuft = false;

const char SEITE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zählwerk</title><style>
:root{--ink:#1B2430;--slate:#2E3B49;--bg:#EEF1F4;--card:#fff;--line:#C4CDD6;
--muted:#5C6B7A;--orange:#FF6B2C;--teal:#7DD3C4}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,sans-serif;padding:16px}
.wrap{max-width:520px;margin:0 auto}
.board{background:#12171D;border-radius:10px;padding:22px 18px;text-align:center}
.score{font:700 74px/1 ui-monospace,Menlo,monospace;color:#F5F7F9;letter-spacing:2px}
.serve{margin-top:10px;font-size:11px;letter-spacing:.16em;color:var(--orange);text-transform:uppercase}
.won{margin-top:8px;color:var(--teal);font-size:13px}
.seq{display:flex;gap:5px;justify-content:center;margin-top:14px;min-height:26px;flex-wrap:wrap}
.chip{width:24px;height:24px;border-radius:4px;display:grid;place-items:center;
font:700 12px ui-monospace,monospace;background:#2E3B49;color:#F5F7F9}
.chip.b{background:var(--teal);color:#12403A}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin-top:14px}
h2{font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:var(--muted);margin-bottom:12px}
.row{display:flex;gap:8px}
button{flex:1;border:1px solid var(--line);background:#fff;border-radius:6px;padding:13px 6px;
font:600 14px inherit;color:var(--ink);cursor:pointer}
button:active{background:#E7ECF0}
button.warn{border-color:var(--orange);color:#B8541F}
label{display:block;font-size:12px;color:var(--muted);margin:12px 0 4px}
input[type=range]{width:100%}
.val{font:600 12px ui-monospace,monospace;color:var(--ink)}
.entry{border-left:2px solid var(--line);padding:4px 0 4px 10px;margin-bottom:10px;font-size:13px}
.entry.w{border-left-color:var(--orange)}
.entry .f{font:700 12px ui-monospace,monospace;color:var(--muted)}
.entry .h{font-size:11.5px;color:#B8541F;margin-top:2px}
.foot{margin-top:14px;text-align:center;font:11px ui-monospace,monospace;color:var(--muted)}
.foot.dirty{color:var(--orange)}
.probe{margin-top:6px;font-size:11px;color:var(--orange)}
.up{display:flex;flex-direction:column;gap:8px}
.up input[type=file],.up input[type=password],.up select{width:100%;padding:9px;
border:1px solid var(--line);border-radius:6px;font:13px inherit;background:#fff}
.bar{height:6px;border-radius:3px;background:#E7ECF0;overflow:hidden;display:none}
.bar>i{display:block;height:100%;width:0;background:var(--orange)}
.msg{font-size:12px;color:var(--muted);min-height:16px}
.net{display:flex;flex-direction:column;gap:8px}
.net input[type=text],.net input[type=password]{width:100%;padding:9px;
border:1px solid var(--line);border-radius:6px;font:13px inherit;background:#fff}
.ni{font:12px/1.7 ui-monospace,monospace;color:var(--muted)}
.ni b{color:var(--ink)}
.ni .warn{color:var(--orange)}
</style></head><body><div class="wrap">

<div class="board">
  <div class="score" id="sc">0:0</div>
  <div class="serve" id="sv">Aufschlag A</div>
  <div class="won" id="wn"></div>
  <div class="seq" id="sq"></div>
</div>

<div class="card"><h2>Korrektur</h2>
  <div class="row">
    <button onclick="go('/punkt?s=A')">Punkt A</button>
    <button onclick="go('/punkt?s=B')">Punkt B</button>
  </div>
  <div class="row" style="margin-top:8px">
    <button class="warn" onclick="go('/zurueck')">Letzten zurück</button>
    <button onclick="go('/neu')">Neues Spiel</button>
  </div>
</div>

<div class="card"><h2>Einstellungen</h2>
  <label>Schwelle Hälfte A — <span class="val" id="la">300</span></label>
  <input type="range" id="ra" min="50" max="2000" step="25" onchange="cfg()">
  <label>Schwelle Hälfte B — <span class="val" id="lb">300</span></label>
  <input type="range" id="rb" min="50" max="2000" step="25" onchange="cfg()">
  <label>Ballwechsel-Timeout — <span class="val" id="lt">1500</span> ms</label>
  <input type="range" id="rt" min="500" max="4000" step="100" onchange="cfg()">
</div>

<div class="card"><h2>Protokoll</h2><div id="lg"></div></div>

<div class="card"><h2>Diagnose</h2>
  <div class="ni" id="di">…</div>
  <div class="net" style="margin-top:10px">
    <label style="margin:0">Sink-Adresse (IP des Laptops)</label>
    <input type="text" id="dh" autocomplete="off" placeholder="192.168.178.188">
    <label style="margin:0">Port</label>
    <input type="text" id="dp" autocomplete="off" placeholder="9000">
    <div class="row">
      <button onclick="diagSpeichern(1)">Speichern und an</button>
      <button class="warn" onclick="diagSpeichern(0)">Aus</button>
    </div>
    <div class="msg" id="dm"></div>
  </div>
</div>

<div class="card"><h2>Netz</h2>
  <div class="ni" id="ni">…</div>
  <div class="net" style="margin-top:10px">
    <label style="margin:0">WLAN-Name</label>
    <input type="text" id="ws" autocomplete="off" placeholder="SSID">
    <label style="margin:0">Passwort</label>
    <input type="password" id="wp" autocomplete="off">
    <label style="margin:0">Wartezeit, bis der eigene Accesspoint aufgeht —
      <span class="val" id="lw">15</span> s</label>
    <input type="range" id="rw" min="5" max="60" step="1" oninput="lw.textContent=rw.value">
    <button onclick="netzSpeichern()">Speichern und neu starten</button>
    <div class="msg" id="nm"></div>
  </div>
</div>

<div class="card"><h2>Firmware einspielen</h2>
  <div class="up">
    <select id="ut">
      <option value="/update">Firmware (firmware.bin)</option>
      <option value="/updatefs">Dateisystem (littlefs.bin)</option>
    </select>
    <input type="file" id="uf" accept=".bin">
    <input type="password" id="up" placeholder="OTA-Passwort" autocomplete="off">
    <button onclick="senden()">Einspielen</button>
    <div class="bar" id="ub"><i id="ubi"></i></div>
    <div class="msg" id="um"></div>
  </div>
</div>

<div class="foot" id="ver"></div>
<div class="probe" id="pb"></div>

</div><script>
let halt=0;
function go(u){halt=Date.now()+400;fetch(u).then(tick)}
function cfg(){
  const a=ra.value,b=rb.value,t=rt.value;
  la.textContent=a;lb.textContent=b;lt.textContent=t;
  halt=Date.now()+400;
  fetch(`/cfg?a=${a}&b=${b}&t=${t}`).then(tick);
}
function tick(){
  if(Date.now()<halt)return;
  fetch('/state').then(r=>r.json()).then(d=>{
    sc.textContent=d.a+':'+d.b;
    sv.textContent=d.over?'Spiel beendet':'Aufschlag '+d.serve;
    wn.textContent=d.over?('Sieger: '+d.winner):'';
    sq.innerHTML=[...d.rally].map(c=>`<div class="chip${c=='B'?' b':''}">${c}</div>`).join('');
    lg.innerHTML=d.log.length?d.log.map(e=>
      `<div class="entry${e.h?' w':''}"><div class="f">${[...e.f].join(' → ')}</div>
       <div>${e.u}</div>${e.h?`<div class="h">${e.h}</div>`:''}</div>`).reverse().join('')
      :'<div style="color:#5C6B7A;font-size:13px">Noch nichts gespielt.</div>';
    pb.textContent = d.img==='pending'
      ? 'Neuer Stand laeuft auf Probe. Bewaehrt er sich, wird er bestaetigt; '
        +'stuerzt er ab, kommt der alte von selbst zurueck.'
      : '';
    if(document.activeElement.type!=='range'){
      ra.value=d.ta;rb.value=d.tb;rt.value=d.to;
      la.textContent=d.ta;lb.textContent=d.tb;lt.textContent=d.to;
    }
  }).catch(()=>{});
}
setInterval(tick,400);tick();
function diagZeigen(d){
  di.innerHTML = (d.on?'Sendet an <b>'+d.host+':'+d.port+'</b>'
                      :'<span class="warn">Aus</span> &mdash; ohne Sink-Adresse wird nichts protokolliert')
    + '<br>Sitzung <b>'+d.session+'</b>'
    + (d.dropped?'<br><span class="warn">'+d.dropped+' Ereignisse verworfen</span>':'');
  if(!dh.value && d.host)dh.value=d.host;
  if(!dp.value)dp.value=d.port;
}
function diagLaden(){fetch('/diag').then(r=>r.json()).then(diagZeigen).catch(()=>{})}
function diagSpeichern(on){
  const q=new URLSearchParams({on:on});
  if(dh.value)q.set('host',dh.value);
  if(dp.value)q.set('port',dp.value);
  halt=Date.now()+400;
  fetch('/diag?'+q).then(r=>r.json()).then(d=>{
    diagZeigen(d);
    dm.textContent = d.on?'Laeuft. Auf dem Laptop muss der Sink lauschen.':'Aus.';
  }).catch(()=>{dm.textContent='Ging nicht.'});
}
function netLaden(){
  fetch('/net').then(r=>r.json()).then(n=>{
    ni.innerHTML =
      (n.mode==='station'
        ? 'Im WLAN <b>'+n.ssid+'</b>'
        : 'Eigener Accesspoint <b>'+n.ssid+'</b>')
      + '<br>Adresse <b>'+n.ip+'</b> &middot; <b>'+n.host+'</b>'
      + '<br>Kanal <b>'+n.ch+'</b> <span class="warn">'
      + '&mdash; ESP-NOW-Taster muessen auf diesem Kanal sitzen</span>';
    if(!ws.value && n.mode==='station')ws.value=n.ssid;
    rw.value=n.to; lw.textContent=n.to;
  }).catch(()=>{});
}
function netzSpeichern(){
  if(!ws.value){nm.textContent='Ohne WLAN-Namen geht es nicht.';return}
  halt=Date.now()+30000;
  const fertig='Gespeichert, das Board startet neu. Danach ist es im neuen WLAN '
    +'zu erreichen &mdash; diese Seite hier nicht mehr. Klappt die Anmeldung '
    +'nicht, spannt es nach der Wartezeit wieder seinen eigenen Accesspoint auf.';
  fetch('/wifi',{method:'POST',
    body:new URLSearchParams({ssid:ws.value,pass:wp.value,to:rw.value})})
    .then(()=>{nm.innerHTML=fertig}).catch(()=>{nm.innerHTML=fertig});
}
function senden(){
  if(!uf.files.length){um.textContent='Erst eine .bin waehlen.';return}
  if(!up.value){um.textContent='Ohne Passwort geht es nicht.';return}
  const fd=new FormData();fd.append('f',uf.files[0]);
  const x=new XMLHttpRequest();
  x.open('POST',ut.value,true);
  x.setRequestHeader('Authorization','Basic '+btoa('zaehlwerk:'+up.value));
  ub.style.display='block';um.textContent='Laedt hoch...';halt=Date.now()+600000;
  x.upload.onprogress=e=>{if(e.lengthComputable)ubi.style.width=(e.loaded/e.total*100)+'%'};
  x.onload=()=>{
    um.textContent=x.status===200
      ?'Eingespielt. Der ESP startet neu — die Seite kommt in ein paar Sekunden zurueck.'
      :(x.status===401?'Passwort falsch.':'Fehlgeschlagen: '+x.responseText);
    if(x.status===200)setTimeout(()=>location.reload(),8000);else halt=0;
  };
  x.onerror=()=>{um.textContent='Verbindung abgebrochen.';halt=0};
  x.send(fd);
}
netLaden();
diagLaden();
setInterval(diagLaden,5000);
fetch('/version').then(r=>r.json()).then(v=>{
  ver.textContent=v.fw+' \u00b7 '+v.git;
  if(v.dirty)ver.classList.add('dirty');
}).catch(()=>{});
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
  j += ",\"to\":" + String(rallyTimeout);
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
           + "\",\"dirty\":" + (ZW_GIT_DIRTY ? "true" : "false") + "}";
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
  Serial.printf("\nZaehlwerk %s  git %s\n", ZW_FW_VERSION, ZW_GIT_HASH);
  Serial.println("Start Game");

  queue = xQueueCreate(16, sizeof(Treffer));
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, NULL, 3, &sensorTaskHandle, 1);

  // Station on the configured network, our own access point if that does not
  // come up in time. Blocks for up to the timeout — deliberately, see net.cpp.
  net::begin({ ZW_HOSTNAME, AP_PREFIX, AP_PASS,
               ZW_STA_SSID, ZW_STA_PASS, ZW_STA_TIMEOUT_MS });

  server.on("/", []{ server.send_P(200, "text/html", SEITE); });
  server.on("/state", handleState);
  server.on("/version", handleVersion);
  server.on("/net", handleNet);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/punkt", []{
    sichern(); rally = "";
    char s = server.arg("s") == "B" ? 'B' : 'A';
    punktGeben(s, "", "Manuell vergeben.", "manual");
    server.send(200, "text/plain", "ok");
  });
  server.on("/zurueck", []{ zurueck(); server.send(200, "text/plain", "ok"); });
  server.on("/neu",     []{ neuesSpiel(); server.send(200, "text/plain", "ok"); });
  server.on("/cfg", []{
    auto setzen = [](const char *arg, const char *name, volatile int &ziel) {
      if (!server.hasArg(arg)) return;
      const int neu = server.arg(arg).toInt();
      if (neu == ziel) return;
      diag::param(name, String(ziel), String(neu), "web");
      ziel = neu;
    };
    setzen("a",  "threshold_a",      schwelleA);
    setzen("b",  "threshold_b",      schwelleB);
    setzen("t",  "rally_timeout_ms", rallyTimeout);
    setzen("la", "log_threshold_a",  logSchwelleA);
    setzen("lb", "log_threshold_b",  logSchwelleB);
    setzen("cr", "clear_ratio",      clearRatioPromille);
    server.send(200, "text/plain", "ok");
  });

  server.on("/diag", []{
    if (server.hasArg("host"))
      diag::setSink(server.arg("host"),
                    server.hasArg("port") ? server.arg("port").toInt() : diag::sinkPort());
    if (server.hasArg("on")) diag::setEnabled(server.arg("on") == "1");
    String j = String("{\"host\":\"") + diag::sinkHost() + "\",\"port\":" +
               diag::sinkPort() + ",\"on\":" + (diag::enabled() ? "true" : "false") +
               ",\"session\":\"" + diag::sessionId() + "\",\"dropped\":" +
               diag::droppedEvents() + "}";
    server.send(200, "application/json", j);
  });
  server.begin();
  webserverLaeuft = true;

  ota::begin(server, { ZW_HOSTNAME, ZW_OTA_PASS, samplingAnhalten, samplingFortsetzen });

  // Last, so the session event carries a network that is already up and the
  // parameters as they actually stand.
  diag::begin({ ZW_DEVICE_ID, "adc", parameterJson(), "boot", parameterJson });
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

void loop() {
  server.handleClient();
  net::tick();
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
