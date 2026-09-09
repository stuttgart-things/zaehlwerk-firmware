/*
  Zählwerk — Stufe 1: Piezo-Charakterisierung
  --------------------------------------------------------------
  Zweck: herausfinden, ob sich ein Ballaufsetzer sauber von
  Schlägerklappern, Anlehnen und Abstellen trennen lässt.

  Hardware: ESP32 (klassisch), ein Piezo an GPIO 34
            mit Schutzbeschaltung (1 MΩ ∥, 100 kΩ Reihe, 2× 1N4148).

  Bedienung im seriellen Monitor, 115200 Baud:
     p  = Plottermodus   (Hüllkurve, für den seriellen Plotter)
     e  = Ereignismodus  (CSV je erkanntem Ereignis)
     r  = Zähler zurücksetzen
     +/- = Schwelle um 50 anheben / senken
*/

const int PIN_PIEZO   = 34;     // ADC1 — bei aktivem WLAN Pflicht
const int FENSTER_US  = 5000;   // 5 ms Hüllkurvenfenster
const int SPERRE_MS   = 60;     // Nachklingen ausblenden
const int MAX_EREIGNIS_MS = 80; // längeres gilt als Dauerdruck, nicht als Schlag

int      schwelle   = 300;      // Startwert, im Betrieb anpassbar
bool     plotter    = true;
uint32_t nummer     = 0;

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);         // 0..4095
  analogSetAttenuation(ADC_11db);   // bis ca. 3,1 V
  delay(300);
  Serial.println();
  Serial.println("Zaehlwerk Stufe 1 - Piezo-Test");
  Serial.println("p = Plotter, e = Ereignisse, r = Reset, +/- = Schwelle");
  Serial.print  ("Schwelle: "); Serial.println(schwelle);
}

void kommandos() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'p') { plotter = true;  Serial.println("-> Plottermodus"); }
    if (c == 'e') { plotter = false; Serial.println("nr,spitze,anstieg_us,dauer_us,pause_ms"); }
    if (c == 'r') { nummer = 0;      Serial.println("-> Zaehler zurueckgesetzt"); }
    if (c == '+') { schwelle += 50;  Serial.print("Schwelle: "); Serial.println(schwelle); }
    if (c == '-') { schwelle -= 50;  if (schwelle < 50) schwelle = 50;
                    Serial.print("Schwelle: "); Serial.println(schwelle); }
  }
}

/* ---------- Plottermodus: Spitzenwert je 5-ms-Fenster ---------- */
void plotterSchritt() {
  uint32_t start = micros();
  int spitze = 0;
  while ((uint32_t)(micros() - start) < FENSTER_US) {
    int v = analogRead(PIN_PIEZO);
    if (v > spitze) spitze = v;
  }
  // Zwei Kurven: Messwert und Schwelle — im Plotter direkt vergleichbar
  Serial.print(spitze);
  Serial.print(',');
  Serial.println(schwelle);
}

/* ---------- Ereignismodus: ein Datensatz je Schlag ---------- */
void ereignisSchritt() {
  static uint32_t letzteMs = 0;

  if (analogRead(PIN_PIEZO) < schwelle) return;

  uint32_t t0      = micros();
  int      spitze  = 0;
  uint32_t tSpitze = t0;
  uint32_t tEnde   = t0;

  // Solange verfolgen, wie das Signal über einem Drittel der Schwelle bleibt.
  // So bekommen wir Anstiegszeit und Abklingdauer mit.
  const int haltePegel = schwelle / 3;
  while ((uint32_t)(micros() - tEnde) < 3000) {          // 3 ms Ruhe = Ende
    int v = analogRead(PIN_PIEZO);
    if (v > spitze) { spitze = v; tSpitze = micros(); }
    if (v > haltePegel) tEnde = micros();
    if ((uint32_t)(micros() - t0) > (uint32_t)MAX_EREIGNIS_MS * 1000) break;
  }

  uint32_t jetzt = millis();
  uint32_t pause = letzteMs ? (jetzt - letzteMs) : 0;
  letzteMs = jetzt;

  Serial.print(++nummer);              Serial.print(',');
  Serial.print(spitze);                Serial.print(',');
  Serial.print(tSpitze - t0);          Serial.print(',');   // Anstiegszeit in µs
  Serial.print(tEnde - t0);            Serial.print(',');   // Dauer in µs
  Serial.println(pause);                                    // Abstand zum Vorgänger

  delay(SPERRE_MS);
}

void loop() {
  kommandos();
  if (plotter) plotterSchritt();
  else         ereignisSchritt();
}

/*
  Auswertung
  --------------------------------------------------------------
  Im Ereignismodus je 10 Wiederholungen aufnehmen von:
    Ball normal / Ball sanft / Ball am Rand
    Schläger ablegen / Schläger klopfen / anlehnen / Tasse / Ball auf den Boden

  Ausgabe in eine Tabelle kopieren und vergleichen:

    Spitze     — trennt die Fälle schon allein?
    anstieg_us — ein Ball ist scharf (kurz), ein abgelegter Schläger weich (lang)

  Gate: schwächster echter Aufsetzer >= 2× stärkster Störer.
  Wird das nur mit Hilfe von anstieg_us erreicht, ist das auch in Ordnung —
  die Firmware kann beide Merkmale nutzen.
*/
