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

  /* Case 1: Ball is served but did not touch side of player serving. */
  /* Example: B Serves      folge => A */
  if (folge[0] != aufschlag)
  {
    u.gewinner = andere(aufschlag);
    u.hinweis = "Erster Aufsetzer nicht auf der Aufschlagseite. Hat der Falsche Spieler gestartet? Bitte prüfen.";
    u.grund = "serve_unclear";

  }
  /* Case 2: Ball is served but did not touch side of recieving player. */
  /* Example: B Serves      folge => B         Punkt A */
  else if (folge.length() == 1)
  {
    u.gewinner = andere(aufschlag);
    u.hinweis = "Nur ein Aufsetzer — Aufschlag ins Aus.";
    u.grund = "serve_fault";
  }

  /* Case 3: Ball is served but hit the net and bounced back. */
  /* Example: B Serves      folge => B B B B   Punkt A */
  else if (folge.All(x => x == aufschlag))
  {
    u.gewinner = andere(aufschlag);
    u.hinweis = "Nur ein Aufsetzer — Aufschlag ins Netz.";
    u.grund = "serve_fault";
  }
  /* Case 4: Ball is served. Ass.*/
  /* Example: B Serves     folge => B A        Punkt A*/
  else if (folge.length() == 2 && folge[0] == aufschlag && folge[1] == andere(aufschlag))
  {
    u.gewinner = aufschlag;
    u.hinweis = "Ass."; // hehe
    u.grund = "single_bounce"
    //hinweis = "Ass oder Netzaufschlag? Nicht unterscheidbar — bitte pruefen.";
  }
  
  /* Case 5: Normal Set  */
  else
  {
    for (unsigned i = 1; i < folge.length(); i++)
    {
      /* Case 5.a - Double Instances. Someone did not hit the ball, hit the net or the ball missed the Table*/
      /* Example: B Serves     folge => B A B A A  Punkt B  */
      /* Example: B Serves     folge => B A B B A  Punkt A  Reason: B hit back even after two bounces. Point still goes to A*/
      /* Example: B Serves     folge => B A A A A  Punkt B  Reason: A hit the Net trying to return the ball.*/
      /* Example: B Serves     folge => B A B B B  Punkt A  Reason: A hit the Net trying to return the ball.*/
      if (folge[i] == folge[i-1]) {
        u.hinweis = String("Doppelaufsetzer auf ") + folge[i] + " — Ball nicht zurueckgespielt.";
        u.gewinner = andere(i);          // Verhindert einen falschen Gewinner, wenn der Ball nach zweimaligem Aufprallen noch zurückgespielt wird.
        u.grund = "double_bounce";
        break;
      }
      /* Case 5.b - Single Instances. Someone did not hit the ball or the ball missed the Table*/
      /* Example: B Serves     folge => B A B A B  Punkt A  */
      else
      {
        u.gewinner = andere(letzte);
        u.grund = "last_bounce";
      }
    }
  }

  return u;
}

}  // namespace game
