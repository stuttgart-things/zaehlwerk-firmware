#include "net.h"

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>

namespace net {
namespace {

// NVS. The names are short because the key length is capped at fifteen
// characters and a truncated key is a silent bug.
const char *NVS_NAMESPACE = "zaehlwerk";
const char *KEY_SSID = "sta_ssid";
const char *KEY_PASS = "sta_pass";
const char *KEY_TIMEOUT = "sta_timeout";

Mode mode_ = Mode::AccessPoint;
String ssid_;
String hostname_;
uint32_t timeout_ = 15000;

void sagen(const String &msg) { Serial.printf("[net] %s\n", msg.c_str()); }

// The chip id in the access point name is what tells two boards in one room
// apart. The last three octets of the station MAC, so it matches the address
// printed everywhere else — d4:e9:f4:c4:e3:2c gives c4e32c.
//
// Read straight out of the efuse rather than through WiFi.macAddress(), which
// asks the station interface. In the access-point-only case that interface was
// never started, the read fails, and every board would be called
// Zaehlwerk-000000.
String apName(const char *prefix) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[32];
  snprintf(buf, sizeof(buf), "%s-%02x%02x%02x", prefix, mac[3], mac[4], mac[5]);
  return String(buf);
}

void openAccessPoint(const Config &cfg) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  ssid_ = apName(cfg.apPrefix);
  WiFi.softAP(ssid_.c_str(), cfg.apPassword);
  mode_ = Mode::AccessPoint;
  sagen("own access point \"" + ssid_ + "\" on " + WiFi.softAPIP().toString() +
      ", channel " + String(WiFi.channel()));
}

}  // namespace

void begin(const Config &cfg) {
  hostname_ = cfg.hostname;

  // isKey() before getString(), because getString() on a key that was never
  // written logs at error level. On a first boot that is the normal state, and
  // two lines of NOT_FOUND read like a fault where there is none.
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  String ssid = prefs.isKey(KEY_SSID) ? prefs.getString(KEY_SSID, "") : String();
  String pass = prefs.isKey(KEY_PASS) ? prefs.getString(KEY_PASS, "") : String();
  timeout_ = prefs.isKey(KEY_TIMEOUT) ? prefs.getUInt(KEY_TIMEOUT, cfg.staTimeoutMs)
                                      : cfg.staTimeoutMs;
  prefs.end();

  // A build flag out of secrets.ini seeds the first connection. It is not
  // copied into NVS: clearing NVS should fall back to the seed rather than to
  // whatever was stored once and then forgotten about.
  bool fromSeed = false;
  if (ssid.isEmpty() && cfg.seedSsid && cfg.seedSsid[0]) {
    ssid = cfg.seedSsid;
    pass = cfg.seedPassword ? cfg.seedPassword : "";
    fromSeed = true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname_.c_str());
  WiFi.setAutoReconnect(true);

  if (ssid.isEmpty()) {
    sagen("no network configured, going straight to the access point");
    openAccessPoint(cfg);
  } else {
    sagen("joining \"" + ssid + "\"" + (fromSeed ? " (from secrets.ini)" : "") +
        ", up to " + String(timeout_ / 1000) + "s");
    WiFi.begin(ssid.c_str(), pass.c_str());

    // waitForConnectResult blocks, which is fine here: this runs in setup(),
    // the sensor task already has its own core, and no game is in progress. A
    // bounce during boot is dropped, which is the right thing to happen to it.
    if (WiFi.waitForConnectResult(timeout_) == WL_CONNECTED) {
      mode_ = Mode::Station;
      ssid_ = ssid;
      sagen("joined, address " + WiFi.localIP().toString() + ", channel " +
          String(WiFi.channel()));
    } else {
      sagen("\"" + ssid + "\" did not come up within the timeout");
      openAccessPoint(cfg);
    }
  }

  // mDNS in both modes, so zaehlwerk.local is one name for both. Resolving it
  // over our own access point is patchy on some clients — the address is on the
  // page for exactly that reason. ArduinoOTA calls MDNS.begin again with the
  // same hostname, which is harmless.
  if (MDNS.begin(hostname_.c_str())) {
    MDNS.addService("http", "tcp", 80);
    sagen("mDNS: " + hostname_ + ".local");
  } else {
    sagen("mDNS did not start");
  }
}

void tick() {}

Mode mode() { return mode_; }

const char *modeName() { return mode_ == Mode::Station ? "station" : "ap"; }

const String &ssid() { return ssid_; }

IPAddress ip() {
  return mode_ == Mode::Station ? WiFi.localIP() : WiFi.softAPIP();
}

int channel() { return WiFi.channel(); }

const String &hostname() { return hostname_; }

uint32_t staTimeout() { return timeout_; }

void setCredentials(const String &ssid, const String &password) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(KEY_SSID, ssid);
  prefs.putString(KEY_PASS, password);
  prefs.end();
  sagen("credentials for \"" + ssid + "\" stored, active on the next start");
}

void setStaTimeout(uint32_t ms) {
  timeout_ = ms;
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUInt(KEY_TIMEOUT, ms);
  prefs.end();
}

bool haveCredentials() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  bool have = prefs.isKey(KEY_SSID) && prefs.getString(KEY_SSID, "").length() > 0;
  prefs.end();
  return have;
}

}  // namespace net
