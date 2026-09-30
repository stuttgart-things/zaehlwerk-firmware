#include "game.h"

#include <cstdlib>

namespace game {

char andere(char seite) { return seite == 'A' ? 'B' : 'A'; }

char aufschlagFuer(int a, int b, char erster) {
  int total = a + b, wechsel;
  if (a >= 10 && b >= 10) wechsel = 10 + (total - 20);
  else                    wechsel = total / 2;
  return (wechsel % 2 == 0) ? erster : andere(erster);
}

bool beendet(int a, int b) { return (a >= 11 || b >= 11) && std::abs(a - b) >= 2; }

Urteil rallyBewerten(const std::string &folge, char aufschlag) {
  Urteil u{' ', "", "none"};
  if (folge.empty()) return u;

  // The base assumption: whoever the ball last bounced on did not get it back,
  // so the point goes to the other side.
  char letzte = folge[folge.length() - 1];
  u.gewinner  = andere(letzte);
  u.grund     = "last_bounce";

  if (folge[0] != aufschlag)
    u.hinweis = "Erster Aufsetzer nicht auf der Aufschlagseite.";

  for (unsigned i = 1; i < folge.length(); i++)
    if (folge[i] == folge[i - 1]) {
      u.hinweis = std::string("Doppelaufsetzer auf ") + folge[i] +
                  " — Ball nicht zurueckgespielt.";
      // Twice in a row on the same half means that side did not get the ball
      // back. The rally ended here and the point belongs to the other side,
      // whatever was measured afterwards. That is exactly why this branch
      // exists rather than the base assumption above: if the ball is played on
      // after the second bounce, this is still what decides.
      u.gewinner = andere(folge[i]);
      u.grund    = "double_bounce";
      break;
    }

  // Two endings the table cannot tell apart. Measured on 2026-09-30: of sixteen
  // points awarded automatically in one game, nine were taken back, and these two
  // rules produced thirteen of them. So they stop awarding: the rally is recorded
  // with its reason, and whoever is playing gives the point by hand.
  //
  // A single bounce is almost never a rally. It is the ball settling after the
  // point was already decided — which is how one stroke scored twice: the timeout
  // ended the rally while the ball was still bouncing, and the next bounce opened
  // a new rally that scored on its own.
  if (folge.length() == 1) {
    u.hinweis  = "Nur ein Aufsetzer — kein Punkt vergeben, bitte selbst geben.";
    u.grund    = "single_bounce";
    u.gewinner = ' ';
  }

  // Serve on the server's half, then the receiver's, then silence. An ace and a
  // serve that clipped the net look identical from underneath the table, and they
  // are not the same thing — one is a point, the other a let.
  if (folge.length() == 2 && folge[0] == aufschlag && folge[1] == andere(aufschlag)) {
    u.hinweis  = "Ass oder Netzaufschlag? Nicht unterscheidbar — kein Punkt.";
    u.grund    = "serve_unclear";
    u.gewinner = ' ';
  }

  return u;
}

}  // namespace game
