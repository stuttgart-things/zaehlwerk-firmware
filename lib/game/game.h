#pragma once

#include <string>

// Die Regeln des Zählwerks, ohne Arduino darunter.
//
// Alles hier ist reine Rechnung auf Zahlen und einer Aufsetzerfolge, damit es
// mit `pio test -e native` auf dem Laptop geprüft werden kann. Wer hier etwas
// ändert, ändert den Spielstand — nicht die Anzeige.
namespace game {

// Die jeweils andere Tischhälfte.
char andere(char seite);

// Wer bei diesem Spielstand aufschlägt. Ab 10:10 wechselt der Aufschlag nach
// jedem Punkt, davor nach jedem zweiten.
char aufschlagFuer(int a, int b, char erster);

// Ein Satz ist durch: elf Punkte und zwei Vorsprung.
bool beendet(int a, int b);

// Das Urteil über einen abgeschlossenen Ballwechsel.
struct Urteil {
  char gewinner;
  std::string hinweis;  // leer, wenn die Folge eindeutig war
};

// Wertet eine Aufsetzerfolge wie "ABAB" aus: Wer bekommt den Punkt, und
// woran ist die Entscheidung unsicher?
Urteil rallyBewerten(const std::string &folge, char aufschlag);

}  // namespace game
