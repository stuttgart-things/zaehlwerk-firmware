#include "ota.h"

#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>

// The Arduino core would mark a freshly uploaded image valid before setup()
// runs. That would make the rollback decoration: confirmed before anything has
// been checked. This override tells the core we decide that ourselves — and it
// has to be extern "C", because the weak symbol comes from esp32-hal-misc.c
// and is declared in no header. Without extern "C" the C++ name is mangled, the
// override does not take, and the mistake only shows up as a rollback test that
// passes for the wrong reason.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {
namespace {

// How long a new build has to run without trouble before it is confirmed. Long
// enough that wifi, the web server and the sensor task really stood up; short
// enough that nobody waits for it.
const uint32_t PROBATION_MS = 10000;

WebServer *server_ = nullptr;
Config cfg_{};
bool enabled_ = false;      // is OTA switched on at all?
bool onProbation_ = false;  // the image is PENDING_VERIFY
bool confirmed_ = false;
int progress_ = -1;
bool rejected_ = false;  // a web upload with no password
bool webFailed_ = false;

void log(const char *msg) { Serial.printf("[ota] %s\n", msg); }

bool authorised() {
  // Basic auth over unencrypted HTTP. That is proportionate for a measuring
  // rig on its own access point and for nothing beyond it.
  return server_->authenticate("zaehlwerk", cfg_.password);
}

// Writing the filesystem partition while it is mounted means overwriting the
// ground a mounted filesystem is standing on: LittleFS holds cached metadata and
// would serve from blocks that no longer say what it thinks. Unmount first.
// If the upload then fails, the page stays gone until a reboot — which is the
// case the recovery page exists for, and it can retry the upload itself.
void unmountForFilesystem(int kind) {
  if (kind != U_SPIFFS) return;
  LittleFS.end();
  Serial.println("[ota] filesystem unmounted for the write");
}

void beginUpdate(int kind) {
  progress_ = 0;
  if (cfg_.pause) cfg_.pause();
  unmountForFilesystem(kind);
  Update.begin(UPDATE_SIZE_UNKNOWN, kind);
}

void handleUpload() {
  HTTPUpload &up = server_->upload();

  switch (up.status) {
    case UPLOAD_FILE_START: {
      rejected_ = false;
      webFailed_ = false;
      if (!enabled_ || !authorised()) {
        rejected_ = true;
        return;
      }
      // The filesystem image comes the same way, just into the other
      // partition. Which kind it is sits in the path rather than in an
      // argument: on a multipart POST the form parser replaces the query
      // arguments, while uri() stays.
      const int kind = server_->uri().endsWith("fs") ? U_SPIFFS : U_FLASH;
      Serial.printf("[ota] web upload starting: %s (%s)\n", up.filename.c_str(),
                    kind == U_SPIFFS ? "filesystem" : "firmware");
      beginUpdate(kind);
      break;
    }

    case UPLOAD_FILE_WRITE:
      if (rejected_ || webFailed_) return;
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        webFailed_ = true;
        Update.printError(Serial);
      }
      break;

    case UPLOAD_FILE_END:
      if (rejected_) return;
      if (webFailed_ || !Update.end(true)) {
        webFailed_ = true;
        Update.printError(Serial);
        progress_ = -1;
        if (cfg_.resume) cfg_.resume();
        return;
      }
      progress_ = 100;
      log("web upload done");
      break;

    default:
      // UPLOAD_FILE_ABORTED
      if (!rejected_) {
        Update.abort();
        progress_ = -1;
        if (cfg_.resume) cfg_.resume();
        log("web upload aborted");
      }
      break;
  }
}

// The bodies below are rendered inside the web UI, which is German. Serial
// output is for whoever is working on the firmware and is English; what a
// person reads in the browser is not.
void handleUploadDone() {
  if (!enabled_) {
    server_->send(503, "text/plain", "OTA ist aus: kein Passwort gesetzt.\n");
    return;
  }
  if (rejected_) {
    server_->requestAuthentication();
    return;
  }
  if (webFailed_) {
    server_->send(500, "text/plain", "Update fehlgeschlagen. Alter Stand laeuft weiter.\n");
    return;
  }
  // Only firmware has a second slot and a rollback. The filesystem is written in
  // place, so promising one here would be a lie at the worst possible moment.
  const bool fs = server_->uri().endsWith("fs");
  server_->send(200, "text/plain",
                fs ? "Dateisystem eingespielt, Neustart. Es gibt nur eine "
                     "Partition: ist das Abbild kaputt, meldet sich die "
                     "Notfallseite und nimmt ein neues an.\n"
                   : "Update eingespielt, Neustart. Laeuft der neue Stand "
                     "nicht, kommt der alte von selbst zurueck.\n");
  delay(200);
  ESP.restart();
}

}  // namespace

void begin(WebServer &server, const Config &cfg) {
  server_ = &server;
  cfg_ = cfg;

  // The partition line is the only way to tell from the monitor which of the
  // two images is running — in a rollback test the version and the git hash are
  // identical on both slots.
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const bool known = running && esp_ota_get_state_partition(running, &state) == ESP_OK;
  onProbation_ = known && state == ESP_OTA_IMG_PENDING_VERIFY;
  if (known && state == ESP_OTA_IMG_VALID) confirmed_ = true;

  const char *word = "unknown";
  if (known) switch (state) {
    case ESP_OTA_IMG_NEW:            word = "new";           break;
    case ESP_OTA_IMG_PENDING_VERIFY: word = "on probation";   break;
    case ESP_OTA_IMG_VALID:          word = "confirmed";      break;
    case ESP_OTA_IMG_INVALID:        word = "invalid";        break;
    case ESP_OTA_IMG_ABORTED:        word = "aborted";        break;
    default:                         word = "unmarked";       break;
  }
  Serial.printf("[ota] running from %s, image %s\n",
                running ? running->label : "?", word);

  // No password, no OTA. An open update path on a wifi you do not control is
  // worse than none — and the message says what to do about it.
  if (cfg.password == nullptr || cfg.password[0] == '\0') {
    log("off: no password. Create secrets.ini (cp secrets.ini.example secrets.ini).");
    server.on("/update", HTTP_POST, handleUploadDone, handleUpload);
    server.on("/updatefs", HTTP_POST, handleUploadDone, handleUpload);
    return;
  }
  enabled_ = true;

  ArduinoOTA.setHostname(cfg.hostname);
  ArduinoOTA.setPassword(cfg.password);

  ArduinoOTA.onStart([] {
    progress_ = 0;
    Serial.printf("[ota] espota starting: %s\n",
                  ArduinoOTA.getCommand() == U_SPIFFS ? "filesystem" : "firmware");
    // It is not the web server that gets in the way here but the sampling:
    // while flash is being written nothing else should want flash and CPU, and
    // the running session belongs ended cleanly rather than cut in half.
    if (cfg_.pause) cfg_.pause();
    unmountForFilesystem(ArduinoOTA.getCommand());
  });
  ArduinoOTA.onProgress([](unsigned int now, unsigned int total) {
    progress_ = total ? (int)((now * 100ULL) / total) : 0;
  });
  ArduinoOTA.onEnd([] {
    progress_ = 100;
    log("espota done, restarting");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    Serial.printf("[ota] error %u, the previous build keeps running\n", err);
    progress_ = -1;
    if (cfg_.resume) cfg_.resume();
  });
  ArduinoOTA.begin();

  server.on("/update", HTTP_POST, handleUploadDone, handleUpload);
  server.on("/updatefs", HTTP_POST, handleUploadDone, handleUpload);
  log("ready");
}

void handle() {
  if (enabled_) ArduinoOTA.handle();
}

void tick(bool healthy) {
  if (!onProbation_ || confirmed_) return;

#ifdef OTA_TEST_CRASH
  // A deliberate crash, to test the rollback. Only on an image that is on
  // probation — a build carrying this flag that was written over USB would
  // otherwise hang in a boot loop nothing can pull it out of.
  if (millis() > 3000) {
    log("OTA_TEST_CRASH: crashing on purpose, the bootloader will roll back");
    delay(50);
    abort();
  }
#endif

  if (!healthy) return;
  if (millis() < PROBATION_MS) return;

  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    confirmed_ = true;
    log("start was clean, image confirmed");
  } else {
    log("could not confirm the image");
  }
}

bool enabled() { return enabled_; }

const char *imageState() {
  if (confirmed_) return "valid";
  if (onProbation_) return "pending";
  return "n/a";
}

int progress() { return progress_; }

}  // namespace ota
