/*
  Zählwerk — Stufe 2: MVP
  --------------------------------------------------------------
  Zwei Piezos an einem ESP32, komplette Zähllogik, Scoreboard als
  Webseite über einen eigenen WLAN-Accesspoint. Kein Router nötig.

  Hardware:
    Piezo Hälfte A -> GPIO 34   (je mit 1 MΩ ∥, 100 kΩ Reihe, 2× 1N4148)
    Piezo Hälfte B -> GPIO 35

  Bedienung:
    WLAN "Zaehlwerk", Passwort "pingpong", dann http://192.168.4.1

  Aufbau der Firmware:
    Core 0  Sensortask, tastet beide Kanäle durch und meldet Ereignisse
    Core 1  Webserver und Spiellogik
  Die Trennung ist wichtig — sonst verschluckt der Webserver Aufsetzer.
*/

#include <WiFi.h>
#include <WebServer.h>

/* ================= Konfiguration ================= */
const int  PIN_A = 34;
const int  PIN_B = 35;
const char *AP_SSID = "Zaehlwerk";
const char *AP_PASS = "pingpong";

volatile int schwelleA   = 300;
volatile int schwelleB   = 300;
volatile int rallyTimeout = 1500;   // ms Stille = Ballwechsel vorbei

const int SPERRE_MS  = 60;          // Nachklingen
const int FENSTER_MS = 30;          // Vergleichsfenster zwischen den Kanälen

/* ================= Sensortask ================= */
struct Treffer { char seite; int spitze; uint32_t t; };
QueueHandle_t queue;

void sensorTask(void *) {
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  uint32_t sperreBis = 0;

  for (;;) {
    if (millis() < sperreBis) { vTaskDelay(1); continue; }

    int a = analogRead(PIN_A);
    int b = analogRead(PIN_B);

    if (a >= schwelleA || b >= schwelleB) {
      // Beide Kanäle für FENSTER_MS verfolgen und die Spitzen sammeln.
      uint32_t start = millis();
      int spA = a, spB = b;
      while (millis() - start < (uint32_t)FENSTER_MS) {
        int va = analogRead(PIN_A); if (va > spA) spA = va;
        int vb = analogRead(PIN_B); if (vb > spB) spB = vb;
      }

      // Relativ zur jeweiligen Schwelle vergleichen — die Kanäle
      // sind nie exakt gleich empfindlich.
      float relA = (float)spA / (float)schwelleA;
      float relB = (float)spB / (float)schwelleB;

      Treffer t;
      t.t = millis();
      if (relA >= relB) { t.seite = 'A'; t.spitze = spA; }
      else              { t.seite = 'B'; t.spitze = spB; }

      xQueueSend(queue, &t, 0);
      sperreBis = millis() + SPERRE_MS;
    }
    vTaskDelay(1);
  }
}

/* ================= Spielzustand ================= */
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

char andere(char s) { return s == 'A' ? 'B' : 'A'; }

char aufschlagFuer(int a, int b, char erst) {
  int total = a + b, wechsel;
  if (a >= 10 && b >= 10) wechsel = 10 + (total - 20);
  else                    wechsel = total / 2;
  return (wechsel % 2 == 0) ? erst : andere(erst);
}
bool beendet(int a, int b) { return (a >= 11 || b >= 11) && abs(a - b) >= 2; }

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
  if (beendet(punkteA, punkteB)) { vorbei = true; sieger = gewinner; }
  else aufschlag = aufschlagFuer(punkteA, punkteB, ersterAufschlag);
  logEintragen(folge, String("Punkt fuer ") + gewinner, hinweis);
}

void rallyBeenden() {
  if (rally.length() == 0) return;
  String folge = rally;
  rally = "";

  char letzte    = folge[folge.length() - 1];
  char gewinner  = andere(letzte);
  String hinweis = "";

  if (folge[0] != aufschlag)
    hinweis = "Erster Aufsetzer nicht auf der Aufschlagseite.";
  for (unsigned i = 1; i < folge.length(); i++)
    if (folge[i] == folge[i-1]) {
      hinweis = String("Doppelaufsetzer auf ") + folge[i] + " — Ball nicht zurueckgespielt.";
      break;
    }
  if (folge.length() == 1)
    hinweis = "Nur ein Aufsetzer — Aufschlag ins Netz oder ins Aus.";
  if (folge.length() == 2 && folge[0] == aufschlag && folge[1] == andere(aufschlag))
    hinweis = "Ass oder Netzaufschlag? Nicht unterscheidbar — bitte pruefen.";

  punktGeben(gewinner, folge, hinweis);
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

/* ================= Webserver ================= */
WebServer server(80);

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
    if(document.activeElement.type!=='range'){
      ra.value=d.ta;rb.value=d.tb;rt.value=d.to;
      la.textContent=d.ta;lb.textContent=d.tb;lt.textContent=d.to;
    }
  }).catch(()=>{});
}
setInterval(tick,400);tick();
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
  j += ",\"log\":[";
  for (int i = 0; i < logAnzahl; i++) {
    if (i) j += ",";
    j += "{\"f\":\"" + log_[i].folge + "\",\"u\":\"" + jsonEscape(log_[i].urteil)
       + "\",\"h\":\"" + jsonEscape(log_[i].hinweis) + "\"}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void setup() {
  Serial.begin(115200);

  queue = xQueueCreate(16, sizeof(Treffer));
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, NULL, 3, NULL, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP laeuft, Adresse: ");
  Serial.println(WiFi.softAPIP());   // 192.168.4.1

  server.on("/", []{ server.send_P(200, "text/html", SEITE); });
  server.on("/state", handleState);
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
}

void loop() {
  server.handleClient();

  Treffer t;
  while (xQueueReceive(queue, &t, 0) == pdTRUE) {
    if (vorbei) continue;
    if (rally.length() == 0) sichern();
    rally += t.seite;
    letzterTreffer = t.t;
    Serial.printf("Treffer %c  Spitze %d\n", t.seite, t.spitze);
  }

  if (rally.length() > 0 && millis() - letzterTreffer > (uint32_t)rallyTimeout)
    rallyBeenden();
}
