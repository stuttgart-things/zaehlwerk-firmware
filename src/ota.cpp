#include "ota.h"

#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_ota_ops.h>

// Der Arduino-Core würde ein frisch eingespieltes Image noch vor setup() als
// gültig markieren. Damit wäre der Rollback Dekoration: bestätigt, bevor
// irgendetwas geprüft wurde. Diese Überschreibung sagt dem Core, dass wir das
// selbst entscheiden — sie muss extern "C" sein, weil das schwache Symbol aus
// esp32-hal-misc.c kommt und ohne Header deklariert ist. Ohne extern "C"
// erzeugt C++ einen anderen Namen, die Überschreibung greift nicht, und der
// Fehler fällt erst beim Rollback-Test auf.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {
namespace {

// Wie lange ein neuer Stand fehlerfrei laufen muss, bevor er bestätigt wird.
// Lang genug, dass WLAN, Webserver und Sensortask wirklich standen; kurz
// genug, dass niemand darauf wartet.
const uint32_t BEWAEHRUNG_MS = 10000;

WebServer *server_ = nullptr;
Config cfg_{};
bool aktiv_ = false;         // OTA überhaupt eingeschaltet?
bool aufProbe_ = false;      // Image ist PENDING_VERIFY
bool bestaetigt_ = false;
int fortschritt_ = -1;
bool abgelehnt_ = false;     // Web-Upload ohne Passwort
bool webFehler_ = false;

void log(const char *was) { Serial.printf("[ota] %s\n", was); }

bool angemeldet() {
  // Basic Auth über unverschlüsseltes HTTP. Das ist für ein Messgerät am
  // eigenen Accesspoint angemessen und für nichts darüber hinaus.
  return server_->authenticate("zaehlwerk", cfg_.password);
}

void updateBeginnt(int art) {
  fortschritt_ = 0;
  if (cfg_.pause) cfg_.pause();
  Update.begin(UPDATE_SIZE_UNKNOWN, art);
}

void handleUpload() {
  HTTPUpload &up = server_->upload();

  switch (up.status) {
    case UPLOAD_FILE_START: {
      abgelehnt_ = false;
      webFehler_ = false;
      if (!aktiv_ || !angemeldet()) {
        abgelehnt_ = true;
        return;
      }
      // Das Dateisystemabbild kommt über denselben Weg, nur in die andere
      // Partition. Die Art steht im Pfad und nicht in einem Argument: bei
      // einem Multipart-POST ersetzt der Formularparser die Query-Argumente,
      // `uri()` bleibt stehen.
      const int art = server_->uri().endsWith("fs") ? U_SPIFFS : U_FLASH;
      Serial.printf("[ota] Web-Upload beginnt: %s (%s)\n", up.filename.c_str(),
                    art == U_SPIFFS ? "Dateisystem" : "Firmware");
      updateBeginnt(art);
      break;
    }

    case UPLOAD_FILE_WRITE:
      if (abgelehnt_ || webFehler_) return;
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        webFehler_ = true;
        Update.printError(Serial);
      }
      break;

    case UPLOAD_FILE_END:
      if (abgelehnt_) return;
      if (webFehler_ || !Update.end(true)) {
        webFehler_ = true;
        Update.printError(Serial);
        fortschritt_ = -1;
        if (cfg_.resume) cfg_.resume();
        return;
      }
      fortschritt_ = 100;
      log("Web-Upload fertig");
      break;

    default:
      // UPLOAD_FILE_ABORTED
      if (!abgelehnt_) {
        Update.abort();
        fortschritt_ = -1;
        if (cfg_.resume) cfg_.resume();
        log("Web-Upload abgebrochen");
      }
      break;
  }
}

void handleUploadFertig() {
  if (!aktiv_) {
    server_->send(503, "text/plain", "OTA ist aus: kein Passwort gesetzt.\n");
    return;
  }
  if (abgelehnt_) {
    server_->requestAuthentication();
    return;
  }
  if (webFehler_) {
    server_->send(500, "text/plain", "Update fehlgeschlagen. Alter Stand laeuft weiter.\n");
    return;
  }
  server_->send(200, "text/plain", "Update eingespielt, Neustart. Laeuft der neue Stand "
                                   "nicht, kommt der alte von selbst zurueck.\n");
  delay(200);
  ESP.restart();
}

}  // namespace

void begin(WebServer &server, const Config &cfg) {
  server_ = &server;
  cfg_ = cfg;

  // Die Partitionszeile ist der einzige Weg, am Monitor zu erkennen, welches
  // der beiden Images gerade läuft — Version und Git-Hash sind bei einem
  // Rollback-Test auf beiden Slots identisch.
  const esp_partition_t *laufend = esp_ota_get_running_partition();
  esp_ota_img_states_t zustand = ESP_OTA_IMG_UNDEFINED;
  const bool bekannt = laufend && esp_ota_get_state_partition(laufend, &zustand) == ESP_OK;
  aufProbe_ = bekannt && zustand == ESP_OTA_IMG_PENDING_VERIFY;
  if (bekannt && zustand == ESP_OTA_IMG_VALID) bestaetigt_ = true;

  const char *wort = "unbekannt";
  if (bekannt) switch (zustand) {
    case ESP_OTA_IMG_NEW:            wort = "neu";                  break;
    case ESP_OTA_IMG_PENDING_VERIFY: wort = "auf Probe";            break;
    case ESP_OTA_IMG_VALID:          wort = "bestaetigt";           break;
    case ESP_OTA_IMG_INVALID:        wort = "verworfen";            break;
    case ESP_OTA_IMG_ABORTED:        wort = "abgebrochen";          break;
    default:                         wort = "ohne Kennzeichnung";   break;
  }
  Serial.printf("[ota] laeuft aus %s, Image %s\n",
                laufend ? laufend->label : "?", wort);

  // Kein Passwort, kein OTA. Ein offener Update-Pfad auf einem fremden WLAN
  // ist schlimmer als gar keiner — und die Meldung sagt, was zu tun ist.
  if (cfg.password == nullptr || cfg.password[0] == '\0') {
    log("aus: kein Passwort. secrets.ini anlegen (cp secrets.ini.example secrets.ini).");
    server.on("/update", HTTP_POST, handleUploadFertig, handleUpload);
    server.on("/updatefs", HTTP_POST, handleUploadFertig, handleUpload);
    return;
  }
  aktiv_ = true;

  ArduinoOTA.setHostname(cfg.hostname);
  ArduinoOTA.setPassword(cfg.password);

  ArduinoOTA.onStart([] {
    fortschritt_ = 0;
    Serial.printf("[ota] espota beginnt: %s\n",
                  ArduinoOTA.getCommand() == U_SPIFFS ? "Dateisystem" : "Firmware");
    // Nicht der Webserver hält hier auf, sondern das Sampling: während
    // geschrieben wird, soll kein Task mehr Flash und CPU beanspruchen, und
    // die laufende Session gehört sauber beendet statt einfach abgeschnitten.
    if (cfg_.pause) cfg_.pause();
  });
  ArduinoOTA.onProgress([](unsigned int jetzt, unsigned int gesamt) {
    fortschritt_ = gesamt ? (int)((jetzt * 100ULL) / gesamt) : 0;
  });
  ArduinoOTA.onEnd([] {
    fortschritt_ = 100;
    log("espota fertig, Neustart");
  });
  ArduinoOTA.onError([](ota_error_t fehler) {
    Serial.printf("[ota] Fehler %u, alter Stand laeuft weiter\n", fehler);
    fortschritt_ = -1;
    if (cfg_.resume) cfg_.resume();
  });
  ArduinoOTA.begin();

  server.on("/update", HTTP_POST, handleUploadFertig, handleUpload);
  server.on("/updatefs", HTTP_POST, handleUploadFertig, handleUpload);
  log("bereit");
}

void handle() {
  if (aktiv_) ArduinoOTA.handle();
}

void tick(bool healthy) {
  if (!aufProbe_ || bestaetigt_) return;

#ifdef OTA_TEST_CRASH
  // Absichtlicher Absturz, um den Rollback zu prüfen. Nur auf einem Image, das
  // auf Probe läuft — ein per USB geflashter Build mit diesem Flag würde sonst
  // in einer Startschleife hängen, aus der ihn nichts holt.
  if (millis() > 3000) {
    log("OTA_TEST_CRASH: Absturz mit Absicht, der Bootloader rollt zurueck");
    delay(50);
    abort();
  }
#endif

  if (!healthy) return;
  if (millis() < BEWAEHRUNG_MS) return;

  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    bestaetigt_ = true;
    log("Start war sauber, Image bestaetigt");
  } else {
    log("Image liess sich nicht bestaetigen");
  }
}

bool enabled() { return aktiv_; }

const char *imageState() {
  if (bestaetigt_) return "valid";
  if (aufProbe_) return "pending";
  return "n/a";
}

int progress() { return fortschritt_; }

}  // namespace ota
