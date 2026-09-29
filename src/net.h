#pragma once

#include <IPAddress.h>
#include <WString.h>
#include <stdint.h>

// Wifi: a station on the configured network as the normal case, the board's own
// access point when that does not come up. See ADR-0002.
namespace net {

enum class Mode { Station, AccessPoint };

struct Config {
  const char *hostname;      // for mDNS and espota
  const char *apPrefix;      // "Zaehlwerk" — the chip id is appended
  const char *apPassword;
  const char *seedSsid;      // from secrets.ini, used only when NVS is empty
  const char *seedPassword;
  uint32_t staTimeoutMs;
};

void begin(const Config &cfg);

// mDNS wants a nudge on some paths. Cheap; belongs next to the web server.
void tick();

Mode mode();
const char *modeName();  // "station" | "ap" — protocol tokens for the web UI

// Which network: the one joined, or the name of our own access point.
const String &ssid();
IPAddress ip();

// The channel the radio ended up on. On the page because ESP-NOW peers have to
// sit on it, and a peer on the wrong channel reports a successful send into
// nothing — which is not diagnosable from the peer.
int channel();

const String &hostname();  // without .local
uint32_t staTimeout();

// Stored in NVS and takes effect on the next start. Deliberately not applied
// live: switching the radio out from under the request that asked for it drops
// the answer, and a wrong password would leave no way back in. A restart runs
// the same path as any boot, and a wrong password lands in the access point
// after the timeout.
void setCredentials(const String &ssid, const String &password);
void setStaTimeout(uint32_t ms);
bool haveCredentials();

}  // namespace net
