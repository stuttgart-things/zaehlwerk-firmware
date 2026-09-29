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
  Urteil u{' ', ""};
  if (folge.empty()) return u;

  // Grundannahme: Wer zuletzt aufsetzen ließ, hat den Ball nicht mehr
  // zurückbekommen — der Punkt geht an die andere Seite.
  char letzte = folge[folge.length() - 1];
  u.gewinner  = andere(letzte);

  if (folge[0] != aufschlag)
    u.hinweis = "Erster Aufsetzer nicht auf der Aufschlagseite.";

  for (unsigned i = 1; i < folge.length(); i++)
    if (folge[i] == folge[i - 1]) {
      u.hinweis = std::string("Doppelaufsetzer auf ") + folge[i] +
                  " — Ball nicht zurueckgespielt.";
      // Zweimal hintereinander auf derselben Hälfte heißt: die Seite hat den
      // Ball nicht zurückbekommen. Der Ballwechsel war hier zu Ende, der
      // Punkt gehört der anderen Seite — und zwar unabhängig davon, was
      // danach noch gemessen wurde. Genau deshalb steht der Zweig hier und
      // nicht in der Grundannahme oben: wird der Ball nach dem zweiten
      // Aufsetzer noch zurückgespielt, entscheidet trotzdem diese Stelle.
      u.gewinner = andere(folge[i]);
      break;
    }

  if (folge.length() == 1)
    u.hinweis = "Nur ein Aufsetzer — Aufschlag ins Netz oder ins Aus.";
  if (folge.length() == 2 && folge[0] == aufschlag && folge[1] == andere(aufschlag))
    u.hinweis = "Ass oder Netzaufschlag? Nicht unterscheidbar — bitte pruefen.";

  return u;
}

}  // namespace game
