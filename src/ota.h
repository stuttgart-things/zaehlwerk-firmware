#pragma once

#include <WebServer.h>

// Getting new firmware onto the board over the network — through PlatformIO
// (espota) and through an upload in the web UI — with a rollback to the
// previous version when the new one does not start cleanly.
namespace ota {

using Hook = void (*)();

struct Config {
  const char *hostname;
  const char *password;  // empty means OTA stays off
  Hook pause;            // suspend sampling, end the session cleanly
  Hook resume;           // only after an update that failed
};

void begin(WebServer &server, const Config &cfg);

// Belongs on the same core as the web server. Has to be called often.
void handle();

// Once per loop. `healthy` is the caller's verdict on its own start; only once
// that has held for a while is the image confirmed and the rollback called off.
void tick(bool healthy);

bool enabled();

// "valid"   — this image is confirmed
// "pending" — on probation, a reset rolls back
// "n/a"     — no OTA information (flashed over USB, for instance)
//
// These are protocol tokens the web UI matches on, not prose.
const char *imageState();

// -1 = no update running, otherwise 0..100
int progress();

}  // namespace ota
