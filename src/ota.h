#pragma once

#include <WebServer.h>

// Einspielen neuer Firmware über das Netz — per PlatformIO (espota) und per
// Upload in der Web-UI — mit Rollback auf die vorige Version, wenn der neue
// Stand nicht sauber startet.
namespace ota {

using Hook = void (*)();

struct Config {
  const char *hostname;
  const char *password;  // leer => OTA bleibt aus
  Hook pause;            // Sampling anhalten, Session sauber beenden
  Hook resume;           // nur nach einem gescheiterten Update
};

void begin(WebServer &server, const Config &cfg);

// Gehört zum Webserver auf denselben Core. Muss oft gerufen werden.
void handle();

// Einmal je Schleifendurchlauf. `healthy` ist das Urteil des Aufrufers über
// den eigenen Start; erst wenn es eine Weile stimmt, wird das Image als
// gültig markiert und der Rollback abgeblasen.
void tick(bool healthy);

bool enabled();

// "valid"  — dieses Image ist bestätigt
// "pending"— läuft auf Probe, ein Reset rollt zurück
// "n/a"    — keine OTA-Information (z. B. per USB geflasht)
const char *imageState();

// -1 = kein Update läuft, sonst 0..100
int progress();

}  // namespace ota
