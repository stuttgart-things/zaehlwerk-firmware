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

#include <string>

#include "config.h"
#include "game.h"
#include "ota.h"
#include "version.h"

/* ================= configuration ================= */
const int  PIN_A = ZW_PIN_A;
const int  PIN_B = ZW_PIN_B;
const char *AP_SSID = ZW_AP_SSID;
const char *AP_PASS = ZW_AP_PASS;

volatile int schwelleA   = 300;
volatile int schwelleB   = 300;
volatile int rallyTimeout = 1500;   // ms Stille = Ballwechsel vorbei

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

void sensorTask(void *) {
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  uint32_t sperreBis = 0;

  for (;;) {
    sensorTicks++;
    if (millis() < sperreBis) { vTaskDelay(1); continue; }

    int a = analogRead(PIN_A);
    int b = analogRead(PIN_B);

    if (a >= schwelleA || b >= schwelleB) {
      // Follow both channels for FENSTER_MS and collect the peaks.
      uint32_t start = millis();
      int spA = a, spB = b;
      while (millis() - start < (uint32_t)FENSTER_MS) {
        int va = analogRead(PIN_A); if (va > spA) spA = va;
        int vb = analogRead(PIN_B); if (vb > spB) spB = vb;
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

      xQueueSend(queue, &t, 0);
      sperreBis = millis() + SPERRE_MS;
    }
    vTaskDelay(1);
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

void punktGeben(char gewinner, String folge, String hinweis) {
  if (gewinner == 'A') punkteA++; else punkteB++;
  if (game::beendet(punkteA, punkteB)) { vorbei = true; sieger = gewinner; }
  else aufschlag = game::aufschlagFuer(punkteA, punkteB, ersterAufschlag);
  logEintragen(folge, String("Punkt fuer ") + gewinner, hinweis);
}

void rallyBeenden() {
  if (rally.length() == 0) return;
  String folge = rally;
  rally = "";

  game::Urteil u = game::rallyBewerten(std::string(folge.c_str()), aufschlag);
  punktGeben(u.gewinner, folge, String(u.hinweis.c_str()));
}

void zurueck() {
  if (verlaufN == 0) return;
  Snapshot s = verlauf[--verlaufN];
  punkteA = s.a; punkteB = s.b; ersterAufschlag = s.erst;
  aufschlag = s.auf; vorbei = s.ende; sieger = s.sieg;
  logAnzahl = s.logN;
  rally = "";
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

void setup() {
  Serial.begin(115200);
  Serial.printf("\nZaehlwerk %s  git %s\n", ZW_FW_VERSION, ZW_GIT_HASH);
  Serial.println("Start Game");

  queue = xQueueCreate(16, sizeof(Treffer));
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, NULL, 3, &sensorTaskHandle, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP up, address: ");
  Serial.println(WiFi.softAPIP());   // 192.168.4.1

  server.on("/", []{ server.send_P(200, "text/html", SEITE); });
  server.on("/state", handleState);
  server.on("/version", handleVersion);
  server.on("/punkt", []{
    sichern(); rally = "";
    char s = server.arg("s") == "B" ? 'B' : 'A';
    punktGeben(s, "", "Manuell vergeben.");
    server.send(200, "text/plain", "ok");
  });
  server.on("/zurueck", []{ zurueck(); server.send(200, "text/plain", "ok"); });
  server.on("/neu",     []{ neuesSpiel(); server.send(200, "text/plain", "ok"); });
  server.on("/cfg", []{
    if (server.hasArg("a")) schwelleA    = server.arg("a").toInt();
    if (server.hasArg("b")) schwelleB    = server.arg("b").toInt();
    if (server.hasArg("t")) rallyTimeout = server.arg("t").toInt();
    server.send(200, "text/plain", "ok");
  });
  server.begin();
  webserverLaeuft = true;

  ota::begin(server, { ZW_HOSTNAME, ZW_OTA_PASS, samplingAnhalten, samplingFortsetzen });
}

// The verdict on our own start that the rollback listens to. Deliberately more
// than "setup() returned": wifi is up, the web server is listening, and the
// sensor task has been round its loop often enough to be running rather than
// merely created.
bool startWarSauber() {
  return webserverLaeuft
      && WiFi.softAPIP() != IPAddress((uint32_t)0)
      && sensorTicks > 1000;
}

void loop() {
  server.handleClient();
  ota::handle();
  ota::tick(startWarSauber());

  Treffer t;
  while (xQueueReceive(queue, &t, 0) == pdTRUE) {
    if (vorbei) continue;
    if (rally.length() == 0) sichern();
    rally += t.seite;
    letzterTreffer = t.t;
    Serial.printf("hit %c    A:%d B:%d\n", t.seite, t.spitzeA, t.spitzeB);
  }

  if (rally.length() > 0 && millis() - letzterTreffer > (uint32_t)rallyTimeout)
    rallyBeenden();
}
