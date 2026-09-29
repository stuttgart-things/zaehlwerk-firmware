#include "mock.h"

#include <Preferences.h>
#include <esp_random.h>
#include <math.h>

namespace mock {
namespace {

const char *NVS_ZW = "zaehlwerk";
const char *KEY_ON = "mock_on";

// A bounce as the front end would pass it on: a sharp rise and a decay, ringing
// while it goes. Not physics — enough shape that the detector's peak window,
// its ratio and its dead time all meet something that behaves like a signal.
const uint32_t DAUER_US = 12000;   // how long one bounce rings
const float ABKLING_US = 4200.0f;  // decay constant
const float FREQ_HZ = 1400.0f;

bool an_ = false;
bool autoplay_ = false;

// The event in flight, as separate volatile scalars rather than a struct.
// The web server writes it on core 0 and the sampler reads it on core 1, and a
// struct copy across that boundary is neither atomic nor legal. `laeuft` is
// written last and read first, so a reader either sees the whole event or none
// of it — the worst case is one sample from just before it started, which is
// indistinguishable from the noise floor anyway.
volatile uint32_t evStartUs_ = 0;
volatile int evAmpA_ = 0, evAmpB_ = 0;
volatile char evSeite_ = 0;
volatile int evArt_ = 0;
volatile bool evLaeuft_ = false;

// What is queued. A rally is a handful of bounces with plausible gaps, then
// silence long enough for the rally timeout to close it.
struct Geplant {
  uint32_t wannMs;
  Kind art;
  char seite;
};
const int PLAN_MAX = 24;
Geplant plan_[PLAN_MAX];
int planN_ = 0, planI_ = 0;

int wuerfel(int von, int bis) {
  return von + (int)(esp_random() % (uint32_t)(bis - von + 1));
}

}  // namespace

const char *kindName(Kind k) {
  switch (k) {
    case Kind::Bounce: return "bounce";
    case Kind::Weak:   return "weak";
    case Kind::Ghost:  return "ghost";
    default:           return "net";
  }
}

void begin(bool vorgabe) {
  Preferences p;
  p.begin(NVS_ZW, true);
  an_ = p.isKey(KEY_ON) ? p.getBool(KEY_ON, vorgabe) : vorgabe;
  p.end();
  Serial.printf("[mock] sensor source: %s\n", an_ ? "mock" : "adc");
}

bool on() { return an_; }

void setOn(bool on) {
  an_ = on;
  autoplay_ = false;
  planN_ = planI_ = 0;
  Preferences p;
  p.begin(NVS_ZW, false);
  p.putBool(KEY_ON, on);
  p.end();
}

int read(bool kanalA, uint32_t nowUs) {
  // A quiet channel is not a silent one. Without a floor the thresholds would
  // mean something here that they do not mean on a table.
  int wert = 6 + (int)(esp_random() % 9);

  if (!evLaeuft_) return wert;

  const uint32_t dt = nowUs - evStartUs_;
  if (dt > DAUER_US) {
    evLaeuft_ = false;
    return wert;
  }
  const int amp = kanalA ? evAmpA_ : evAmpB_;
  if (amp <= 0) return wert;

  const float huelle = expf(-(float)dt / ABKLING_US);
  const float schwingung = sinf((float)dt * 1e-6f * FREQ_HZ * 6.2832f);
  return wert + (int)((float)amp * huelle * fabsf(schwingung));
}

void trigger(Kind k, char seite) {
  int nah = 0, fern = 0;
  switch (k) {
    case Kind::Bounce:
      nah = wuerfel(900, 1900);
      // Crosstalk: a single-piece table passes a good part of it to the other
      // half, which is why the two channels are compared at all.
      fern = nah / wuerfel(3, 6);
      break;
    case Kind::Weak:
      // Deliberately astride the threshold. Half of these should be missed,
      // and that is the point of having them.
      nah = wuerfel(260, 340);
      fern = nah / 4;
      break;
    case Kind::Ghost:
      // Nothing happened. One channel, short and loud enough to cross.
      nah = wuerfel(350, 600);
      fern = 0;
      break;
    case Kind::Net:
      // Both halves equally loud. The ratio cannot separate this, and a
      // detector that claims a side here is guessing.
      nah = wuerfel(700, 1200);
      fern = nah - wuerfel(0, 60);
      break;
  }
  evLaeuft_ = false;   // stop any reader before the fields move under it
  evAmpA_ = (seite == 'A') ? nah : fern;
  evAmpB_ = (seite == 'A') ? fern : nah;
  evSeite_ = seite;
  evArt_ = (int)k;
  evStartUs_ = micros();
  evLaeuft_ = true;    // last, so the event is whole when it becomes visible
}

void playRally() {
  planN_ = planI_ = 0;
  uint32_t t = millis() + 200;
  char seite = 'A';
  const int aufsetzer = wuerfel(2, 7);
  for (int i = 0; i < aufsetzer && planN_ < PLAN_MAX; i++) {
    plan_[planN_++] = { t, Kind::Bounce, seite };
    // The gap between two bounces of a real exchange.
    t += wuerfel(240, 520);
    seite = (seite == 'A') ? 'B' : 'A';
  }
}

void setAutoplay(bool on) {
  autoplay_ = on;
  if (on && planI_ >= planN_) playRally();
}

bool autoplay() { return autoplay_; }

void tick() {
  if (!an_) return;
  const uint32_t jetzt = millis();
  while (planI_ < planN_ && jetzt >= plan_[planI_].wannMs) {
    trigger(plan_[planI_].art, plan_[planI_].seite);
    planI_++;
  }
  // Wait out the rally timeout before the next one, or the log would see one
  // endless exchange instead of a game.
  if (autoplay_ && planI_ >= planN_ && jetzt > plan_[planN_ ? planN_ - 1 : 0].wannMs + 2200) {
    playRally();
  }
}

char intendedSide() { return evLaeuft_ ? evSeite_ : 0; }

const char *intendedType() {
  return evLaeuft_ ? kindName((Kind)evArt_) : nullptr;
}

}  // namespace mock
