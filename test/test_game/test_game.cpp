// Tests der Spielregeln.

#include <unity.h>

#include <string>

#include "game.h"

void setUp() {}
void tearDown() {}

/* ---------- Aufschlagwechsel ---------- */

static void aufschlag_wechselt_nach_je_zwei_punkten() {
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(0, 0, 'A'));
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(1, 0, 'A'));
  TEST_ASSERT_EQUAL_CHAR('B', game::aufschlagFuer(1, 1, 'A'));
  TEST_ASSERT_EQUAL_CHAR('B', game::aufschlagFuer(2, 1, 'A'));
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(2, 2, 'A'));
}

static void ab_zehn_zu_zehn_wechselt_der_aufschlag_nach_jedem_punkt() {
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(10, 10, 'A'));
  TEST_ASSERT_EQUAL_CHAR('B', game::aufschlagFuer(11, 10, 'A'));
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(11, 11, 'A'));
}

static void der_erste_aufschlag_dreht_die_ganze_reihe() {
  TEST_ASSERT_EQUAL_CHAR('B', game::aufschlagFuer(0, 0, 'B'));
  TEST_ASSERT_EQUAL_CHAR('A', game::aufschlagFuer(1, 1, 'B'));
}

/* ---------- Satzende ---------- */

static void elf_punkte_mit_zwei_vorsprung_beenden_den_satz() {
  TEST_ASSERT_TRUE(game::beendet(11, 9));
  TEST_ASSERT_TRUE(game::beendet(9, 11));
  TEST_ASSERT_FALSE(game::beendet(10, 9));
}

static void bei_einem_punkt_vorsprung_geht_es_weiter() {
  TEST_ASSERT_FALSE(game::beendet(11, 10));
  TEST_ASSERT_TRUE(game::beendet(12, 10));
  TEST_ASSERT_FALSE(game::beendet(15, 14));
}

/* ---------- Auswertung eines Ballwechsels ---------- */

static void der_punkt_geht_an_die_seite_des_vorletzten_aufsetzers() {
  game::Urteil u = game::rallyBewerten("ABAB", 'A');
  TEST_ASSERT_EQUAL_CHAR('A', u.gewinner);
  TEST_ASSERT_TRUE(u.hinweis.empty());
}

static void ein_einzelner_aufsetzer_ist_ein_verpatzter_aufschlag() {
  game::Urteil u = game::rallyBewerten("A", 'A');
  TEST_ASSERT_EQUAL_CHAR('B', u.gewinner);
  TEST_ASSERT_TRUE(u.hinweis.find("Nur ein Aufsetzer") != std::string::npos);
}

static void zwei_aufsetzer_ab_aufschlag_sind_nicht_eindeutig() {
  game::Urteil u = game::rallyBewerten("AB", 'A');
  TEST_ASSERT_EQUAL_CHAR('A', u.gewinner);
  TEST_ASSERT_TRUE(u.hinweis.find("Ass oder Netzaufschlag") != std::string::npos);
}

static void ein_erster_aufsetzer_auf_der_falschen_seite_wird_vermerkt() {
  game::Urteil u = game::rallyBewerten("BABA", 'A');
  TEST_ASSERT_EQUAL_CHAR('B', u.gewinner);
  TEST_ASSERT_TRUE(u.hinweis.find("nicht auf der Aufschlagseite") != std::string::npos);
}

static void eine_leere_folge_vergibt_keinen_punkt() {
  game::Urteil u = game::rallyBewerten("", 'A');
  TEST_ASSERT_EQUAL_CHAR(' ', u.gewinner);
}

// Zwei Aufsetzer hintereinander auf derselben Hälfte heißt: der Ball kam nicht
// zurück, der Punkt gehört der anderen Seite.
static void ein_doppelaufsetzer_wird_als_solcher_erkannt() {
  game::Urteil u = game::rallyBewerten("ABAA", 'A');
  TEST_ASSERT_TRUE(u.hinweis.find("Doppelaufsetzer") != std::string::npos);
}

static void der_punkt_geht_an_die_seite_ohne_den_doppelaufsetzer() {
  // Doppelaufsetzer auf A am Ende — A hat nicht zurueckbekommen.
  TEST_ASSERT_EQUAL_CHAR('B', game::rallyBewerten("ABAA", 'A').gewinner);
  // Dasselbe auf B.
  TEST_ASSERT_EQUAL_CHAR('A', game::rallyBewerten("BABB", 'B').gewinner);
}

// Der Fall, fuer den der Zweig ueberhaupt existiert: nach dem zweiten
// Aufsetzer wurde noch gespielt. Der Ballwechsel war trotzdem vorher zu Ende,
// also darf der letzte Aufsetzer nicht mehr entscheiden.
static void ein_doppelaufsetzer_entscheidet_auch_wenn_danach_weitergespielt_wird() {
  // "ABBA": Doppelaufsetzer auf B, Punkt fuer A. Nach dem letzten Aufsetzer
  // allein waere es 'B' — hier gewinnt die Stelle des Doppelaufsetzers.
  TEST_ASSERT_EQUAL_CHAR('A', game::rallyBewerten("ABBA", 'A').gewinner);
  // "BAAB": Doppelaufsetzer auf A, Punkt fuer B; nach dem letzten Aufsetzer
  // waere es 'A'.
  TEST_ASSERT_EQUAL_CHAR('B', game::rallyBewerten("BAAB", 'B').gewinner);
}

// Der erste Doppelaufsetzer entscheidet, nicht der letzte — danach ist der
// Ballwechsel vorbei und alles Weitere ist Messrauschen.
static void der_erste_doppelaufsetzer_entscheidet() {
  TEST_ASSERT_EQUAL_CHAR('B', game::rallyBewerten("AABB", 'A').gewinner);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(aufschlag_wechselt_nach_je_zwei_punkten);
  RUN_TEST(ab_zehn_zu_zehn_wechselt_der_aufschlag_nach_jedem_punkt);
  RUN_TEST(der_erste_aufschlag_dreht_die_ganze_reihe);
  RUN_TEST(elf_punkte_mit_zwei_vorsprung_beenden_den_satz);
  RUN_TEST(bei_einem_punkt_vorsprung_geht_es_weiter);
  RUN_TEST(der_punkt_geht_an_die_seite_des_vorletzten_aufsetzers);
  RUN_TEST(ein_einzelner_aufsetzer_ist_ein_verpatzter_aufschlag);
  RUN_TEST(zwei_aufsetzer_ab_aufschlag_sind_nicht_eindeutig);
  RUN_TEST(ein_erster_aufsetzer_auf_der_falschen_seite_wird_vermerkt);
  RUN_TEST(eine_leere_folge_vergibt_keinen_punkt);
  RUN_TEST(ein_doppelaufsetzer_wird_als_solcher_erkannt);
  RUN_TEST(der_punkt_geht_an_die_seite_ohne_den_doppelaufsetzer);
  RUN_TEST(ein_doppelaufsetzer_entscheidet_auch_wenn_danach_weitergespielt_wird);
  RUN_TEST(der_erste_doppelaufsetzer_entscheidet);
  return UNITY_END();
}
