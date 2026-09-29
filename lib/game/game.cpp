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

  if (folge.length() == 1) {
    u.hinweis = "Nur ein Aufsetzer — Aufschlag ins Netz oder ins Aus.";
    u.grund   = "single_bounce";
  }
  if (folge.length() == 2 && folge[0] == aufschlag && folge[1] == andere(aufschlag))
    u.hinweis = "Ass oder Netzaufschlag? Nicht unterscheidbar — bitte pruefen.";

  return u;
}

}  // namespace game
