/*
===============================================================
  Projekt:    fischertechnik Hochregallager mit ftDuino
  Datei:      Hochregallager.ino
  Autor:      HR

  GitHub:     HR-Projekte
===============================================================
  Version: 16.06.2026
  Steuerprogramm fuer ein fischertechnik-Hochregallager mit ftDuino.
  Das Programm bedient Turm, Wagen, Gabel und Umsetzer, fuehrt eine
  Referenzfahrt aus und ermoeglicht manuelle sowie automatische Lagerablaeufe.
  Ueber OLED-Display und I2C-Tastenfeld werden Betriebsart, Lagerart,
  Einlagerung und Entnahme ausgewaehlt; Sensoren und Impulszaehler dienen zur
  Positionsbestimmung und zur Absicherung der Bewegungen.
===============================================================  
*/

#include <Ftduino.h>
#include "Wire.h"
#include "I2CKeyPad.h"
#include "SSD1306Ascii.h"
#include "SSD1306AsciiAvrI2c.h"
#include "Antriebsmodul.h"
constexpr uint8_t I2C_ADDRESS = 0X3D; // Speziell für dieses Display, Standard 0X3C
// Bei Bedarf auf 1 setzen, um die Serial-Diagnose wieder einzuschalten.
#define DEBUG_AUSGABE 0

#if !DEBUG_AUSGABE
struct StummerSerial {
  template <typename T>
  void begin(const T&) {}

  template <typename T>
  void print(const T&) {}

  template <typename T>
  void println(const T&) {}

  void println() {}
};

StummerSerial debugSerial;
#define Serial debugSerial
#endif

constexpr uint8_t KEYPAD_ADDRESS = 0x20;
constexpr char KEYPAD_KEYS[] PROGMEM = "123A456B789C*0#DNF";
constexpr char KEINE_TASTE = 'N';

constexpr char TASTE_BETRIEBSART = 'A';
constexpr char TASTE_REFERENZFAHRT = 'D';
constexpr char TASTE_LAGERART = 'B';
constexpr char TASTE_AKTION_START = '#';
constexpr char TASTE_AUTOMATIK_EINLAGERN = '3';
constexpr char TASTE_AUTOMATIK_ENTNAHME_LIFO = '6';
constexpr char TASTE_AUTOMATIK_ENTNAHME_FIFO = '9';
constexpr char TASTE_ESC = 'C';
constexpr unsigned long STARTUP_DELAY_MS = 1000;
constexpr unsigned long SERIAL_BAUD = 115200;

constexpr int GABEL_MOTOR = Ftduino::M2;
constexpr int GABEL_HINTEN_ENDSTOP = Ftduino::I2;
constexpr int GABEL_VORNE_ENDSTOP = Ftduino::I4;
constexpr int UMSETZER_SENSOR = Ftduino::I5;
constexpr unsigned long UMSETZER_SENSOR_ENTPRELL_MS = 1000;

constexpr int TURM_PORT = Ftduino::M3;
constexpr int TURM_SPEED = Ftduino::MAX;

constexpr int TURM_BASIS_OBEN = 3910;   // 3855 rechnerisch bei gleichen
constexpr int TURM_BASIS_MITTE = 1970;  // abständen unten-mitte, mitte-oben.
constexpr int TURM_BASIS_UNTEN = 85;

constexpr int HUB_IMPULSE = 275;
constexpr int TURM_COUNT_MAX = TURM_BASIS_OBEN + HUB_IMPULSE;

constexpr int WAGEN_PORT = Ftduino::M1;
constexpr int WAGEN_SPEED = Ftduino::MAX;

constexpr int WAGEN_LINKS = 6045;
constexpr int WAGEN_RECHTS = 9845;
constexpr int WAGEN_COUNT_MAX = WAGEN_RECHTS;

constexpr int UMSETZ_TURM_BASIS = 0; // Entspricht Enschalter I3 (referenz)
constexpr int UMSETZ_WAGEN_POS = 0;  // Entspricht Enschalter I1 (referenz)

struct ZielPos {
  int turmBasis;
  int wagenPos;
};

constexpr ZielPos UMSETZPLATZ = {
  UMSETZ_TURM_BASIS,
  UMSETZ_WAGEN_POS
};

SSD1306AsciiAvrI2c oled;
I2CKeyPad keyPad(KEYPAD_ADDRESS);
Antriebsmodul turm(TURM_PORT, TURM_COUNT_MAX, TURM_SPEED);
Antriebsmodul wagen(WAGEN_PORT, WAGEN_COUNT_MAX, WAGEN_SPEED);

bool keypadBereit = false;
bool tasteGehalten = false;
bool referenziert = false;
bool referenzOkHinweisAktiv = false;
bool referenzDisplayInitialisiert = false;
bool umsetzerSensorBereit = false;
bool umsetzerSensorHiAktiv = false;
unsigned long umsetzerSensorHiSeit = 0;
int lagerWare[6] = {0, 0, 0, 0, 0, 0};
int naechsteWareId = 1;
int ausgewaehlterLagerplatz = 0;
int aktiverEinlagernPlatz = 0;
int aktiverAuslagernPlatz = 0;
int umsetzenVonPlatz = 0;
int umsetzenNachPlatz = 0;
int aktiverUmsetzenVonPlatz = 0;
int aktiverUmsetzenNachPlatz = 0;
int automatikAnzeigePlatz = 0;
bool aktiverUmsetzenTauscht = false;

enum Betriebsart {
  BETRIEBSART_MANUELL,
  BETRIEBSART_AUTOMATIK
};

enum Lagerart {
  LAGERART_EINLAGERN,
  LAGERART_AUSLAGERN,
  LAGERART_UMSETZEN
};

enum AutomatikAuswahl {
  AUTOMATIK_KEINE,
  AUTOMATIK_EINLAGERN,
  AUTOMATIK_AUSLAGERN_LIFO,
  AUTOMATIK_AUSLAGERN_FIFO
};

Betriebsart betriebsart = BETRIEBSART_MANUELL;
Lagerart lagerart = LAGERART_EINLAGERN;
AutomatikAuswahl automatikAuswahl = AUTOMATIK_KEINE;

// Ablauf der gesamten Referenzfahrt:
// Erst wird die Gabel nach hinten gefahren, danach starten Turm und Wagen
// ihre normale Linksbewegung bis zum jeweiligen Endschalter.
enum ReferenzStatus {
  REFERENZ_AUS,
  REFERENZ_GABEL,
  REFERENZ_ACHSEN
};

ReferenzStatus referenzStatus = REFERENZ_AUS;
bool turmReferenziert = false;
bool wagenReferenziert = false;

enum EinlagernStatus {
  EINLAGERN_AUS,
  EINLAGERN_FAHRE_UMSETZPLATZ,
  EINLAGERN_GABEL_ZUM_UMSETZPLATZ,
  EINLAGERN_HEBE_VOM_UMSETZPLATZ,
  EINLAGERN_GABEL_VOM_UMSETZPLATZ,
  EINLAGERN_FAHRE_LAGERPLATZ,
  EINLAGERN_GABEL_ZUM_LAGERPLATZ,
  EINLAGERN_SENKE_AM_LAGERPLATZ,
  EINLAGERN_GABEL_VOM_LAGERPLATZ,
  EINLAGERN_FAHRE_REFERENZ
};

EinlagernStatus einlagernStatus = EINLAGERN_AUS;

enum AuslagernStatus {
  AUSLAGERN_AUS,
  AUSLAGERN_FAHRE_LAGERPLATZ,
  AUSLAGERN_GABEL_ZUM_LAGERPLATZ,
  AUSLAGERN_HEBE_VOM_LAGERPLATZ,
  AUSLAGERN_GABEL_VOM_LAGERPLATZ,
  AUSLAGERN_FAHRE_UMSETZPLATZ,
  AUSLAGERN_GABEL_ZUM_UMSETZPLATZ,
  AUSLAGERN_SENKE_AM_UMSETZPLATZ,
  AUSLAGERN_GABEL_VOM_UMSETZPLATZ,
  AUSLAGERN_FAHRE_REFERENZ
};

AuslagernStatus auslagernStatus = AUSLAGERN_AUS;

enum UmsetzenStatus {
  UMSETZEN_AUS,
  UMSETZEN_FAHRE_QUELLPLATZ,
  UMSETZEN_GABEL_ZUM_QUELLPLATZ,
  UMSETZEN_HEBE_VOM_QUELLPLATZ,
  UMSETZEN_GABEL_VOM_QUELLPLATZ,
  UMSETZEN_FAHRE_UMSETZPLATZ_MIT_QUELLE,
  UMSETZEN_GABEL_ZUM_UMSETZPLATZ_QUELLE,
  UMSETZEN_SENKE_AM_UMSETZPLATZ_QUELLE,
  UMSETZEN_GABEL_VOM_UMSETZPLATZ_QUELLE,
  UMSETZEN_FAHRE_ZIEL_ZUM_TAUSCHEN,
  UMSETZEN_GABEL_ZUM_ZIEL_ZUM_TAUSCHEN,
  UMSETZEN_HEBE_VOM_ZIEL_ZUM_TAUSCHEN,
  UMSETZEN_GABEL_VOM_ZIEL_ZUM_TAUSCHEN,
  UMSETZEN_FAHRE_QUELLE_ZUM_ABLEGEN,
  UMSETZEN_GABEL_ZUR_QUELLE_ZUM_ABLEGEN,
  UMSETZEN_SENKE_AN_QUELLE,
  UMSETZEN_GABEL_VON_QUELLE,
  UMSETZEN_FAHRE_UMSETZPLATZ_ZUM_AUFNEHMEN,
  UMSETZEN_GABEL_ZUM_UMSETZPLATZ_AUFNEHMEN,
  UMSETZEN_HEBE_VOM_UMSETZPLATZ,
  UMSETZEN_GABEL_VOM_UMSETZPLATZ,
  UMSETZEN_FAHRE_ZIELPLATZ,
  UMSETZEN_GABEL_ZUM_ZIELPLATZ,
  UMSETZEN_SENKE_AM_ZIELPLATZ,
  UMSETZEN_GABEL_VOM_ZIELPLATZ,
  UMSETZEN_FAHRE_REFERENZ
};

UmsetzenStatus umsetzenStatus = UMSETZEN_AUS;

struct TastaturStatus {
  char taste;
  bool flanke;
};

// ==================  Funktionsprototypen  ==================
// Arduino-Hauptfunktionen
void setup();
void loop();

// Display und Ausgabe
void initialisiereDisplay();
void displayIntro();
void displayReferenzHinweis();
void displayReferenzOkHinweis();
void displayReferenzfahrtStatus();
void displayReferenzEndschalterZeile(const __FlashStringHelper* name, int nameLen, bool erreicht);
void gibBetriebsartAus();
void gibLagerartAus();
void displayLagerart();
char lagerplatzAnzeige(int platzNummer);
char lagerplatzAnzeigeAuslagern(int platzNummer);
char lagerplatzAnzeigeUmsetzen(int platzNummer);
void displayLagerplatzZeile(const __FlashStringHelper* ebene, int links, int rechts);
void displayLagerplatzZeileAuslagern(const __FlashStringHelper* ebene, int links, int rechts);
void displayLagerplatzZeileUmsetzen(const __FlashStringHelper* ebene, int links, int rechts);
void displayManuell();
void displayUmsetzenStatuszeile();
void displayAutomatik();
void aktualisiereDisplay();
void gibBerichtAus();

// Tastatur und Menue
char leseTaste();
TastaturStatus leseTastatur();
void wechsleBetriebsart();
void wechsleLagerart();
void loescheMenueEingaben();
bool istLagerplatzTaste(char taste);
int lagerplatzAusTaste(char taste);
void waehleLagerplatz(int platzNummer);

// Lagerverwaltung und Positionen
bool lagerplatzGueltig(int platzNummer);
bool lagerplatzBelegt(int platzNummer);
int wareAufLagerplatz(int platzNummer);
void lagereNeueWareEin(int platzNummer);
void entferneWareAusLagerplatz(int platzNummer);
void verschiebeWare(int vonPlatz, int nachPlatz);
void tauscheWare(int ersterPlatz, int zweiterPlatz);
bool alleLagerplaetzeBelegt();
int freierLagerplatz();
int lifoLagerplatz();
int fifoLagerplatz();
int lagerPosY(int platzNummer);
int lagerPosX(int platzNummer);
ZielPos lagerplatzPos(int platzNummer);

// Gabel, Einlagern und Auslagern
bool gabelIstHinten();
bool gabelIstVorne();
void stopGabel();
void fahreGabelVor();
void fahreGabelZurueck();
bool umsetzerIstBelegt();
void bearbeiteAutomatikEinlagerSensor();
bool einlagernAktiv();
void starteEinlagern();
void bearbeiteEinlagern();
bool auslagernAktiv();
void starteAuslagern();
void bearbeiteAuslagern();
bool umsetzenAktiv();
void starteUmsetzen();
void bearbeiteUmsetzen();
void starteAutomatikEinlagern();
void starteAutomatikAuslagernLifo();
void starteAutomatikAuslagernFifo();

// Referenzfahrt
bool referenzpositionErreicht();
void pruefeReferenzpositionBeimStart();
bool referenzfahrtAktiv();
void starteReferenzfahrt();
void stoppeReferenzfahrt();
void bearbeiteReferenzfahrt();

// ==================  Arduino-Hauptfunktionen  ==================
void setup() {
 
  delay(STARTUP_DELAY_MS);
  Serial.begin(SERIAL_BAUD);

  ftduino.init();
  Wire.begin();
  initialisiereDisplay();

  ftduino.input_set_mode(GABEL_HINTEN_ENDSTOP, Ftduino::SWITCH);
  ftduino.input_set_mode(GABEL_VORNE_ENDSTOP, Ftduino::SWITCH);
  ftduino.input_set_mode(UMSETZER_SENSOR, Ftduino::SWITCH);

  turm.init();
  wagen.init();

  keypadBereit = keyPad.begin();
  turm.stop();
  wagen.stop();
  stopGabel();

  displayIntro();
  pruefeReferenzpositionBeimStart();
  aktualisiereDisplay();

}

void loop() {
  TastaturStatus tastatur = leseTastatur();

  if (referenzfahrtAktiv() && tastatur.taste != TASTE_REFERENZFAHRT) {
    stoppeReferenzfahrt();
  }

  if (tastatur.flanke) {
    if (referenzOkHinweisAktiv && tastatur.taste == TASTE_BETRIEBSART) {
      referenzOkHinweisAktiv = false;
      aktualisiereDisplay();
    }
    else if (tastatur.taste == TASTE_REFERENZFAHRT) {
      starteReferenzfahrt();
    }
    else if (tastatur.taste == TASTE_BETRIEBSART) {
      wechsleBetriebsart();
    }
    else if (tastatur.taste == TASTE_LAGERART && betriebsart == BETRIEBSART_MANUELL) {
      wechsleLagerart();
    }
    else if (tastatur.taste == TASTE_ESC) {
      loescheMenueEingaben();
    }
    else if (betriebsart == BETRIEBSART_MANUELL &&
             istLagerplatzTaste(tastatur.taste)) {
      waehleLagerplatz(lagerplatzAusTaste(tastatur.taste));
    }
    else if (tastatur.taste == TASTE_AKTION_START &&
             betriebsart == BETRIEBSART_MANUELL &&
             lagerart == LAGERART_EINLAGERN) {
      starteEinlagern();
    }
    else if (tastatur.taste == TASTE_AKTION_START &&
             betriebsart == BETRIEBSART_MANUELL &&
             lagerart == LAGERART_AUSLAGERN) {
      starteAuslagern();
    }
    else if (tastatur.taste == TASTE_AKTION_START &&
             betriebsart == BETRIEBSART_MANUELL &&
             lagerart == LAGERART_UMSETZEN) {
      starteUmsetzen();
    }
    else if (tastatur.taste == TASTE_AUTOMATIK_ENTNAHME_LIFO &&
             betriebsart == BETRIEBSART_AUTOMATIK) {
      starteAutomatikAuslagernLifo();
    }
    else if (tastatur.taste == TASTE_AUTOMATIK_ENTNAHME_FIFO &&
             betriebsart == BETRIEBSART_AUTOMATIK) {
      starteAutomatikAuslagernFifo();
    }
    else if (tastatur.taste == TASTE_AUTOMATIK_EINLAGERN &&
             betriebsart == BETRIEBSART_AUTOMATIK) {
      starteAutomatikEinlagern();
    }
  }

  if (referenzfahrtAktiv()) {
    bearbeiteReferenzfahrt();
  }
  else if (einlagernAktiv()) {
    bearbeiteEinlagern();
  }
  else if (auslagernAktiv()) {
    bearbeiteAuslagern();
  }
  else if (umsetzenAktiv()) {
    bearbeiteUmsetzen();
  }

  bearbeiteAutomatikEinlagerSensor();

  turm.update();
  wagen.update();

  delay(20);
}

// ==================  Intro  ==========================
void initialisiereDisplay() {
  oled.begin(&Adafruit128x64, I2C_ADDRESS);
  oled.setFont(Adafruit5x7);
  oled.clear();
}

void displayIntro() {
  oled.clear();
  oled.println();
  oled.println(F("    Hochregallager   "));
  oled.println();
  oled.println(F("         mit         "));
  oled.println();
  oled.println(F("       ftDuino       "));

  delay(6000);
  oled.clear();
}

void displayReferenzHinweis() {
  oled.clear();

  oled.setCursor(18, 1);
  oled.print(F("Referenzfahrt"));

  oled.setCursor(28, 3);
  oled.print(F("mit Taste "));
  oled.println(TASTE_REFERENZFAHRT);
}

void displayReferenzOkHinweis() {
  oled.clear();
  oled.println(F("H I N W E I S !     "));
  oled.println();
  oled.println(F("Referenz ok"));
  oled.println();
  oled.println(F("Menue-Anwahl mit A  "));
  oled.println(F("> Manuell"));
  oled.println(F("> Automatik"));
  oled.println();
}

void displayReferenzfahrtStatus() {
  if (!referenzDisplayInitialisiert) {
    oled.clear();
    oled.println(F("Referenzfahrt"));
    oled.println();
    referenzDisplayInitialisiert = true;
  }

  oled.setCursor(0, 2);
  displayReferenzEndschalterZeile(F("Greifer"), 7, gabelIstHinten());
  oled.setCursor(0, 3);
  displayReferenzEndschalterZeile(F("Turm"), 4, turm.referenzErreicht());
  oled.setCursor(0, 4);
  displayReferenzEndschalterZeile(F("Wagen"), 5, wagen.referenzErreicht());
}

void displayReferenzEndschalterZeile(const __FlashStringHelper* name, int nameLen, bool erreicht) {
  oled.print(name);

  for (int s = nameLen; s < 12; s++) {
    oled.print(' ');
  }

  if (erreicht) {
    oled.println(F("O"));
  }
  else {
    oled.println(F("X"));
  }
}

bool gabelIstHinten() {
  return ftduino.input_get(GABEL_HINTEN_ENDSTOP);
}

bool gabelIstVorne() {
  return ftduino.input_get(GABEL_VORNE_ENDSTOP);
}

bool umsetzerIstBelegt() {
  return ftduino.input_get(UMSETZER_SENSOR);
}

void bearbeiteAutomatikEinlagerSensor() {
  if (betriebsart != BETRIEBSART_AUTOMATIK ||
      automatikAuswahl != AUTOMATIK_KEINE ||
      !referenziert ||
      referenzOkHinweisAktiv ||
      referenzfahrtAktiv() ||
      einlagernAktiv() ||
      auslagernAktiv() ||
      umsetzenAktiv()) {
    umsetzerSensorBereit = false;
    umsetzerSensorHiAktiv = false;
    return;
  }

  if (!umsetzerIstBelegt()) {
    umsetzerSensorBereit = true;
    umsetzerSensorHiAktiv = false;
    return;
  }

  if (!umsetzerSensorBereit) {
    return;
  }

  unsigned long jetzt = millis();
  if (!umsetzerSensorHiAktiv) {
    umsetzerSensorHiAktiv = true;
    umsetzerSensorHiSeit = jetzt;
    return;
  }

  if (jetzt - umsetzerSensorHiSeit >= UMSETZER_SENSOR_ENTPRELL_MS) {
    umsetzerSensorBereit = false;
    umsetzerSensorHiAktiv = false;
    starteAutomatikEinlagern();
  }
}

char leseTaste() {
  if (!keypadBereit) {
    return KEINE_TASTE;
  }

  uint8_t key = keyPad.getKey();

  if (key >= sizeof(KEYPAD_KEYS) - 1) {
    return KEINE_TASTE;
  }

  return pgm_read_byte(&KEYPAD_KEYS[key]);
}

TastaturStatus leseTastatur() {
  TastaturStatus status = {leseTaste(), false};

  if (status.taste != KEINE_TASTE && !tasteGehalten) {
    tasteGehalten = true;
    status.flanke = true;
  }

  if (status.taste == KEINE_TASTE) {
    tasteGehalten = false;
  }

  return status;
}

void gibBetriebsartAus() {
  Serial.print(F("Betriebsart: "));
  if (betriebsart == BETRIEBSART_MANUELL) {
    Serial.println(F("Manuell"));
  }
  else {
    Serial.println(F("Automatik"));
  }
}

void gibLagerartAus() {
  Serial.print(F("Lagerart: "));
  if (lagerart == LAGERART_EINLAGERN) {
    Serial.println(F("E i n l a g e r n"));
  }
  else if (lagerart == LAGERART_AUSLAGERN) {
    Serial.println(F("A u s l a g e r n"));
  }
  else if (lagerart == LAGERART_UMSETZEN) {
    Serial.println(F("U m s e t z e n"));
  }
}

void displayLagerart() {
  if (lagerart == LAGERART_EINLAGERN) {
    oled.println(F("E i n l a g e r n"));
  }
  else if (lagerart == LAGERART_AUSLAGERN) {
    oled.println(F("A u s l a g e r n"));
  }
  else if (lagerart == LAGERART_UMSETZEN) {
    oled.println(F("U m s e t z e n"));
  }
}

bool lagerplatzGueltig(int platzNummer) {
  return platzNummer >= 1 && platzNummer <= 6;
}

bool lagerplatzBelegt(int platzNummer) {
  if (!lagerplatzGueltig(platzNummer)) {
    return false;
  }

  return lagerWare[platzNummer - 1] != 0;
}

int wareAufLagerplatz(int platzNummer) {
  if (!lagerplatzGueltig(platzNummer)) {
    return 0;
  }

  return lagerWare[platzNummer - 1];
}

void lagereNeueWareEin(int platzNummer) {
  if (!lagerplatzGueltig(platzNummer)) {
    return;
  }

  lagerWare[platzNummer - 1] = naechsteWareId;
  naechsteWareId++;
}

void entferneWareAusLagerplatz(int platzNummer) {
  if (!lagerplatzGueltig(platzNummer)) {
    return;
  }

  lagerWare[platzNummer - 1] = 0;
}

void verschiebeWare(int vonPlatz, int nachPlatz) {
  if (!lagerplatzGueltig(vonPlatz) || !lagerplatzGueltig(nachPlatz)) {
    return;
  }

  lagerWare[nachPlatz - 1] = lagerWare[vonPlatz - 1];
  lagerWare[vonPlatz - 1] = 0;
}

void tauscheWare(int ersterPlatz, int zweiterPlatz) {
  if (!lagerplatzGueltig(ersterPlatz) || !lagerplatzGueltig(zweiterPlatz)) {
    return;
  }

  int ware = lagerWare[ersterPlatz - 1];
  lagerWare[ersterPlatz - 1] = lagerWare[zweiterPlatz - 1];
  lagerWare[zweiterPlatz - 1] = ware;
}

bool alleLagerplaetzeBelegt() {
  for (int i = 0; i < 6; i++) {
    if (lagerWare[i] == 0) {
      return false;
    }
  }

  return true;
}

int freierLagerplatz() {
  for (int platzNummer = 1; platzNummer <= 6; platzNummer++) {
    if (!lagerplatzBelegt(platzNummer)) {
      return platzNummer;
    }
  }

  return 0;
}

int lifoLagerplatz() {
  int platz = 0;
  int neuesteWare = 0;

  for (int platzNummer = 1; platzNummer <= 6; platzNummer++) {
    int ware = wareAufLagerplatz(platzNummer);
    if (ware > neuesteWare) {
      neuesteWare = ware;
      platz = platzNummer;
    }
  }

  return platz;
}

int fifoLagerplatz() {
  int platz = 0;
  int aeltesteWare = naechsteWareId;

  for (int platzNummer = 1; platzNummer <= 6; platzNummer++) {
    int ware = wareAufLagerplatz(platzNummer);
    if (ware > 0 && ware < aeltesteWare) {
      aeltesteWare = ware;
      platz = platzNummer;
    }
  }

  return platz;
}

char lagerplatzAnzeige(int platzNummer) {
  if (lagerplatzBelegt(platzNummer)) {
    return 'X';
  }

  return '0' + platzNummer;
}

char lagerplatzAnzeigeAuslagern(int platzNummer) {
  if (!lagerplatzBelegt(platzNummer)) {
    return 'X';
  }

  return '0' + platzNummer;
}

char lagerplatzAnzeigeUmsetzen(int platzNummer) {
  if (umsetzenVonPlatz == 0) {
    return lagerplatzAnzeigeAuslagern(platzNummer);
  }

  if (platzNummer == umsetzenVonPlatz) {
    return 'Q';
  }

  return '0' + platzNummer;
}

void displayLagerplatzZeile(const __FlashStringHelper* ebene, int links, int rechts) {
  oled.print(ebene);
  oled.print(F("        "));
  oled.print(lagerplatzAnzeige(links));
  oled.print(F(" | "));
  oled.println(lagerplatzAnzeige(rechts));
}

void displayLagerplatzZeileAuslagern(const __FlashStringHelper* ebene, int links, int rechts) {
  oled.print(ebene);
  oled.print(F("        "));
  oled.print(lagerplatzAnzeigeAuslagern(links));
  oled.print(F(" | "));
  oled.println(lagerplatzAnzeigeAuslagern(rechts));
}

void displayLagerplatzZeileUmsetzen(const __FlashStringHelper* ebene, int links, int rechts) {
  oled.print(ebene);
  oled.print(F("        "));
  oled.print(lagerplatzAnzeigeUmsetzen(links));
  oled.print(F(" | "));
  oled.println(lagerplatzAnzeigeUmsetzen(rechts));
}

void displayManuell() {
  oled.clear();
  oled.println(F("M a n u e l l"));
  oled.println(F("Lagerart B / Esc C"));
  oled.println();
  displayLagerart();

  if (lagerart == LAGERART_EINLAGERN) {
    displayLagerplatzZeile(F("Oben "), 1, 2);
    displayLagerplatzZeile(F("Mitte"), 3, 4);
    displayLagerplatzZeile(F("Unten"), 5, 6);

    if (alleLagerplaetzeBelegt()) {
      oled.println(F("Es ist alles belegt!"));
    }
    else if (ausgewaehlterLagerplatz == 0) {
      oled.println(F("Setze U -> _ Start #"));
    }
    else if (lagerplatzBelegt(ausgewaehlterLagerplatz)) {
      oled.print(F("Platz "));
      oled.print(ausgewaehlterLagerplatz);
      oled.println(F(" ist belegt"));
    }
    else {
      oled.print(F("Setze U -> "));
      oled.print(ausgewaehlterLagerplatz);
      oled.println(F(" Start #"));
    }
  }
  else if (lagerart == LAGERART_AUSLAGERN) {
    displayLagerplatzZeileAuslagern(F("Oben "), 1, 2);
    displayLagerplatzZeileAuslagern(F("Mitte"), 3, 4);
    displayLagerplatzZeileAuslagern(F("Unten"), 5, 6);

    if (ausgewaehlterLagerplatz == 0) {
      oled.println(F("Setze U <- _ Start #"));
    }
    else if (!lagerplatzBelegt(ausgewaehlterLagerplatz)) {
      oled.print(F("Platz "));
      oled.print(ausgewaehlterLagerplatz);
      oled.println(F(" ist leer"));
    }
    else {
      oled.print(F("Setze U <- "));
      oled.print(ausgewaehlterLagerplatz);
      oled.println(F(" Start #"));
    }
  }
  else {
    displayLagerplatzZeileUmsetzen(F("Oben "), 1, 2);
    displayLagerplatzZeileUmsetzen(F("Mitte"), 3, 4);
    displayLagerplatzZeileUmsetzen(F("Unten"), 5, 6);
    displayUmsetzenStatuszeile();
  }
}

void displayUmsetzenStatuszeile() {
  int vonPlatz = umsetzenAktiv() ? aktiverUmsetzenVonPlatz : umsetzenVonPlatz;
  int nachPlatz = umsetzenAktiv() ? aktiverUmsetzenNachPlatz : umsetzenNachPlatz;

  oled.print(F("Setze "));

  if (vonPlatz == 0) {
    oled.print(F("_"));
  }
  else {
    oled.print(vonPlatz);
  }

  if (umsetzenAktiv()) {
    if (aktiverUmsetzenTauscht) {
      oled.print(F(" <> "));
    }
    else {
      oled.print(F(" -> "));
    }
  }
  else if (vonPlatz != 0 && nachPlatz != 0) {
    oled.print(F(" -- "));
  }
  else if (vonPlatz != 0) {
    oled.print(F(" -? "));
  }
  else {
    oled.print(F(" ?? "));
  }

  if (nachPlatz == 0) {
    oled.print(F("_"));
  }
  else {
    oled.print(nachPlatz);
  }

  oled.println(F(" Start #"));
}

void displayAutomatik() {
  oled.clear();
  oled.println(F("A u t o m a t i k"));
  oled.println();
  oled.println(F("Einlagern 3 o I5"));
  oled.println(F("Entnahme  LIFO 6"));
  oled.println(F("Entnahme  FIFO 9"));
  oled.println();
  oled.println();

  if (automatikAuswahl == AUTOMATIK_EINLAGERN) {
    if (automatikAnzeigePlatz == 0) {
      oled.println(F("Setze U -> Platz _"));
    }
    else {
      oled.print(F("Setze U -> Platz "));
      oled.println(automatikAnzeigePlatz);
    }
  }
  else if (automatikAuswahl == AUTOMATIK_AUSLAGERN_LIFO ||
           automatikAuswahl == AUTOMATIK_AUSLAGERN_FIFO) {
    if (automatikAnzeigePlatz == 0) {
      oled.println(F("Setze U <- Platz _"));
    }
    else {
      oled.print(F("Setze U <- Platz "));
      oled.println(automatikAnzeigePlatz);
    }
  }
  else {
    oled.println(F("Setze U -> Platz _"));
  }
}

void aktualisiereDisplay() {
  if (!referenziert && !referenzfahrtAktiv()) {
    displayReferenzHinweis();
    return;
  }

  if (referenzOkHinweisAktiv) {
    displayReferenzOkHinweis();
    return;
  }

  if (betriebsart == BETRIEBSART_MANUELL) {
    displayManuell();
  }
  else {
    displayAutomatik();
  }
}

void wechsleBetriebsart() {
  if (betriebsart == BETRIEBSART_MANUELL) {
    betriebsart = BETRIEBSART_AUTOMATIK;
  }
  else {
    betriebsart = BETRIEBSART_MANUELL;
  }

  ausgewaehlterLagerplatz = 0;
  umsetzenVonPlatz = 0;
  umsetzenNachPlatz = 0;
  automatikAuswahl = AUTOMATIK_KEINE;
  automatikAnzeigePlatz = 0;
  gibBetriebsartAus();
  aktualisiereDisplay();
}

void wechsleLagerart() {
  if (lagerart == LAGERART_EINLAGERN) {
    lagerart = LAGERART_AUSLAGERN;
  }
  else if (lagerart == LAGERART_AUSLAGERN) {
    lagerart = LAGERART_UMSETZEN;
  }
  else {
    lagerart = LAGERART_EINLAGERN;
  }

  gibLagerartAus();
  ausgewaehlterLagerplatz = 0;
  umsetzenVonPlatz = 0;
  umsetzenNachPlatz = 0;
  aktualisiereDisplay();
}

void loescheMenueEingaben() {
  ausgewaehlterLagerplatz = 0;
  umsetzenVonPlatz = 0;
  umsetzenNachPlatz = 0;
  automatikAuswahl = AUTOMATIK_KEINE;
  automatikAnzeigePlatz = 0;
  Serial.println(F("Eingaben geloescht"));
  gibLagerartAus();
  aktualisiereDisplay();
}

void stopGabel() {
  ftduino.motor_set(GABEL_MOTOR, Ftduino::BRAKE, Ftduino::MAX);
}

void fahreGabelVor() {
  if (gabelIstVorne()) {
    stopGabel();
    return;
  }

  ftduino.motor_set(GABEL_MOTOR, Ftduino::RIGHT, Ftduino::MAX);
}

void fahreGabelZurueck() {
  if (gabelIstHinten()) {
    stopGabel();
    return;
  }

  ftduino.motor_set(GABEL_MOTOR, Ftduino::LEFT, Ftduino::MAX);
}

void gibBerichtAus() {
  Serial.println(F("Bericht"));
  Serial.print(F("Turm: "));
  Serial.println(turm.getPosition());
  Serial.print(F("Wagen: "));
  Serial.println(wagen.getPosition());
}

int lagerPosY(int platzNummer) {
  if (platzNummer <= 2) {
    return TURM_BASIS_OBEN;
  }

  if (platzNummer <= 4) {
    return TURM_BASIS_MITTE;
  }

  return TURM_BASIS_UNTEN;
}

int lagerPosX(int platzNummer) {
  if (platzNummer % 2 == 1) {
    return WAGEN_LINKS;
  }
  return WAGEN_RECHTS;
}

ZielPos lagerplatzPos(int platzNummer) {
  ZielPos pos = {
    lagerPosY(platzNummer),
    lagerPosX(platzNummer)
  };

  return pos;
}

bool istLagerplatzTaste(char taste) {
  return taste >= '1' && taste <= '6';
}

int lagerplatzAusTaste(char taste) {
  if (!istLagerplatzTaste(taste)) {
    return 0;
  }

  return taste - '0';
}

void waehleLagerplatz(int platzNummer) {
  if (!lagerplatzGueltig(platzNummer)) {
    return;
  }

  if (lagerart == LAGERART_UMSETZEN) {
    if (umsetzenVonPlatz == 0) {
      if (!lagerplatzBelegt(platzNummer)) {
        Serial.print(F("Quelle "));
        Serial.print(platzNummer);
        Serial.println(F(" ist leer"));
        aktualisiereDisplay();
        return;
      }

      umsetzenVonPlatz = platzNummer;
      Serial.print(F("Umsetzen Quelle: "));
      Serial.println(umsetzenVonPlatz);
      aktualisiereDisplay();
      return;
    }

    if (platzNummer == umsetzenVonPlatz) {
      Serial.println(F("Quelle und Ziel sind gleich"));
      aktualisiereDisplay();
      return;
    }

    umsetzenNachPlatz = platzNummer;
    Serial.print(F("Umsetzen Ziel: "));
    Serial.println(umsetzenNachPlatz);
    aktualisiereDisplay();
    return;
  }

  ausgewaehlterLagerplatz = platzNummer;

  Serial.print(F("Lagerplatz gewaehlt: "));
  Serial.println(ausgewaehlterLagerplatz);

  if (lagerplatzBelegt(ausgewaehlterLagerplatz)) {
    Serial.print(F("Platz "));
    Serial.print(ausgewaehlterLagerplatz);
    Serial.println(F(" ist belegt"));
  }

  aktualisiereDisplay();
}

bool einlagernAktiv() {
  return einlagernStatus != EINLAGERN_AUS;
}

void starteEinlagern() {
  if (ausgewaehlterLagerplatz == 0) {
    if (alleLagerplaetzeBelegt()) {
      Serial.println(F("Es ist alles belegt!"));
      aktualisiereDisplay();
      return;
    }

    Serial.println(F("Bitte zuerst Lagerplatz waehlen"));
    aktualisiereDisplay();
    return;
  }

  if (lagerplatzBelegt(ausgewaehlterLagerplatz)) {
    Serial.print(F("Platz "));
    Serial.print(ausgewaehlterLagerplatz);
    Serial.println(F(" ist belegt"));
    aktualisiereDisplay();
    return;
  }

  aktiverEinlagernPlatz = ausgewaehlterLagerplatz;

  Serial.print(F("Einlagern Platz "));
  Serial.println(aktiverEinlagernPlatz);

  ZielPos umsetzplatz = UMSETZPLATZ;
  turm.setArbeitsposition(umsetzplatz.turmBasis);
  wagen.setArbeitsposition(umsetzplatz.wagenPos);
  einlagernStatus = EINLAGERN_FAHRE_UMSETZPLATZ;
}

void starteAuslagern() {
  if (ausgewaehlterLagerplatz == 0) {
    Serial.println(F("Bitte zuerst Lagerplatz waehlen"));
    aktualisiereDisplay();
    return;
  }

  if (!lagerplatzBelegt(ausgewaehlterLagerplatz)) {
    Serial.print(F("Platz "));
    Serial.print(ausgewaehlterLagerplatz);
    Serial.println(F(" ist leer"));
    aktualisiereDisplay();
    return;
  }

  aktiverAuslagernPlatz = ausgewaehlterLagerplatz;

  Serial.print(F("Auslagern Platz "));
  Serial.println(aktiverAuslagernPlatz);

  ZielPos lagerplatz = lagerplatzPos(aktiverAuslagernPlatz);
  turm.setArbeitsposition(lagerplatz.turmBasis);
  wagen.setArbeitsposition(lagerplatz.wagenPos);
  auslagernStatus = AUSLAGERN_FAHRE_LAGERPLATZ;
}

void starteAutomatikEinlagern() {
  automatikAuswahl = AUTOMATIK_EINLAGERN;
  int platz = freierLagerplatz();

  if (platz == 0) {
    Serial.println(F("Es ist alles belegt!"));
    aktualisiereDisplay();
    return;
  }

  ausgewaehlterLagerplatz = platz;
  automatikAnzeigePlatz = platz;
  aktualisiereDisplay();
  starteEinlagern();
}

void starteAutomatikAuslagernLifo() {
  automatikAuswahl = AUTOMATIK_AUSLAGERN_LIFO;
  int platz = lifoLagerplatz();

  if (platz == 0) {
    Serial.println(F("Keine Ware zum Auslagern"));
    aktualisiereDisplay();
    return;
  }

  ausgewaehlterLagerplatz = platz;
  automatikAnzeigePlatz = platz;
  aktualisiereDisplay();
  starteAuslagern();
}

void starteAutomatikAuslagernFifo() {
  automatikAuswahl = AUTOMATIK_AUSLAGERN_FIFO;
  int platz = fifoLagerplatz();

  if (platz == 0) {
    Serial.println(F("Keine Ware zum Auslagern"));
    aktualisiereDisplay();
    return;
  }

  ausgewaehlterLagerplatz = platz;
  automatikAnzeigePlatz = platz;
  aktualisiereDisplay();
  starteAuslagern();
}

void bearbeiteEinlagern() {
  ZielPos lagerplatz = lagerplatzPos(aktiverEinlagernPlatz);

  if (einlagernStatus == EINLAGERN_FAHRE_UMSETZPLATZ) {
    if (turm.istFertig() && wagen.istFertig()) {
      einlagernStatus = EINLAGERN_GABEL_ZUM_UMSETZPLATZ;
    }
  }
  else if (einlagernStatus == EINLAGERN_GABEL_ZUM_UMSETZPLATZ) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(UMSETZPLATZ.turmBasis + HUB_IMPULSE);
      einlagernStatus = EINLAGERN_HEBE_VOM_UMSETZPLATZ;
    }
  }
  else if (einlagernStatus == EINLAGERN_HEBE_VOM_UMSETZPLATZ) {
    if (turm.istFertig()) {
      einlagernStatus = EINLAGERN_GABEL_VOM_UMSETZPLATZ;
    }
  }
  else if (einlagernStatus == EINLAGERN_GABEL_VOM_UMSETZPLATZ) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turm.setArbeitsposition(lagerplatz.turmBasis + HUB_IMPULSE);
      wagen.setArbeitsposition(lagerplatz.wagenPos);
      einlagernStatus = EINLAGERN_FAHRE_LAGERPLATZ;
    }
  }
  else if (einlagernStatus == EINLAGERN_FAHRE_LAGERPLATZ) {
    if (turm.istFertig() && wagen.istFertig()) {
      einlagernStatus = EINLAGERN_GABEL_ZUM_LAGERPLATZ;
    }
  }
  else if (einlagernStatus == EINLAGERN_GABEL_ZUM_LAGERPLATZ) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(lagerplatz.turmBasis);
      einlagernStatus = EINLAGERN_SENKE_AM_LAGERPLATZ;
    }
  }
  else if (einlagernStatus == EINLAGERN_SENKE_AM_LAGERPLATZ) {
    if (turm.istFertig()) {
      einlagernStatus = EINLAGERN_GABEL_VOM_LAGERPLATZ;
    }
  }
  else if (einlagernStatus == EINLAGERN_GABEL_VOM_LAGERPLATZ) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turmReferenziert = false;
      wagenReferenziert = false;
      turm.referenzStart();
      wagen.referenzStart();
      einlagernStatus = EINLAGERN_FAHRE_REFERENZ;
    }
  }
  else if (einlagernStatus == EINLAGERN_FAHRE_REFERENZ) {
    if (!turmReferenziert && turm.referenzErreicht()) {
      turm.stop();
      turm.referenzSetzen();
      turmReferenziert = true;
    }

    if (!wagenReferenziert && wagen.referenzErreicht()) {
      wagen.stop();
      wagen.referenzSetzen();
      wagenReferenziert = true;
    }

    if (turmReferenziert && wagenReferenziert) {
      einlagernStatus = EINLAGERN_AUS;
      lagereNeueWareEin(aktiverEinlagernPlatz);
      ausgewaehlterLagerplatz = 0;
      aktiverEinlagernPlatz = 0;
      automatikAuswahl = AUTOMATIK_KEINE;
      automatikAnzeigePlatz = 0;
      Serial.println(F("Einlagern beendet"));
      gibBerichtAus();
      aktualisiereDisplay();
    }
  }
}

bool auslagernAktiv() {
  return auslagernStatus != AUSLAGERN_AUS;
}

void bearbeiteAuslagern() {
  ZielPos lagerplatz = lagerplatzPos(aktiverAuslagernPlatz);

  if (auslagernStatus == AUSLAGERN_FAHRE_LAGERPLATZ) {
    if (turm.istFertig() && wagen.istFertig()) {
      auslagernStatus = AUSLAGERN_GABEL_ZUM_LAGERPLATZ;
    }
  }
  else if (auslagernStatus == AUSLAGERN_GABEL_ZUM_LAGERPLATZ) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(lagerplatz.turmBasis + HUB_IMPULSE);
      auslagernStatus = AUSLAGERN_HEBE_VOM_LAGERPLATZ;
    }
  }
  else if (auslagernStatus == AUSLAGERN_HEBE_VOM_LAGERPLATZ) {
    if (turm.istFertig()) {
      auslagernStatus = AUSLAGERN_GABEL_VOM_LAGERPLATZ;
    }
  }
  else if (auslagernStatus == AUSLAGERN_GABEL_VOM_LAGERPLATZ) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turm.setArbeitsposition(UMSETZPLATZ.turmBasis + HUB_IMPULSE);
      wagen.setArbeitsposition(UMSETZPLATZ.wagenPos);
      auslagernStatus = AUSLAGERN_FAHRE_UMSETZPLATZ;
    }
  }
  else if (auslagernStatus == AUSLAGERN_FAHRE_UMSETZPLATZ) {
    if (turm.istFertig() && wagen.istFertig()) {
      auslagernStatus = AUSLAGERN_GABEL_ZUM_UMSETZPLATZ;
    }
  }
  else if (auslagernStatus == AUSLAGERN_GABEL_ZUM_UMSETZPLATZ) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(UMSETZPLATZ.turmBasis);
      auslagernStatus = AUSLAGERN_SENKE_AM_UMSETZPLATZ;
    }
  }
  else if (auslagernStatus == AUSLAGERN_SENKE_AM_UMSETZPLATZ) {
    if (turm.istFertig()) {
      auslagernStatus = AUSLAGERN_GABEL_VOM_UMSETZPLATZ;
    }
  }
  else if (auslagernStatus == AUSLAGERN_GABEL_VOM_UMSETZPLATZ) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turmReferenziert = false;
      wagenReferenziert = false;
      turm.referenzStart();
      wagen.referenzStart();
      auslagernStatus = AUSLAGERN_FAHRE_REFERENZ;
    }
  }
  else if (auslagernStatus == AUSLAGERN_FAHRE_REFERENZ) {
    if (!turmReferenziert && turm.referenzErreicht()) {
      turm.stop();
      turm.referenzSetzen();
      turmReferenziert = true;
    }

    if (!wagenReferenziert && wagen.referenzErreicht()) {
      wagen.stop();
      wagen.referenzSetzen();
      wagenReferenziert = true;
    }

    if (turmReferenziert && wagenReferenziert) {
      auslagernStatus = AUSLAGERN_AUS;
      entferneWareAusLagerplatz(aktiverAuslagernPlatz);
      ausgewaehlterLagerplatz = 0;
      aktiverAuslagernPlatz = 0;
      automatikAuswahl = AUTOMATIK_KEINE;
      automatikAnzeigePlatz = 0;
      Serial.println(F("Auslagern beendet"));
      gibBerichtAus();
      aktualisiereDisplay();
    }
  }
}

bool umsetzenAktiv() {
  return umsetzenStatus != UMSETZEN_AUS;
}

void starteUmsetzen() {
  if (umsetzenVonPlatz == 0) {
    Serial.println(F("Bitte zuerst Quelle waehlen"));
    aktualisiereDisplay();
    return;
  }

  if (umsetzenNachPlatz == 0) {
    Serial.println(F("Bitte zuerst Ziel waehlen"));
    aktualisiereDisplay();
    return;
  }

  if (!lagerplatzBelegt(umsetzenVonPlatz)) {
    Serial.print(F("Quelle "));
    Serial.print(umsetzenVonPlatz);
    Serial.println(F(" ist leer"));
    aktualisiereDisplay();
    return;
  }

  aktiverUmsetzenVonPlatz = umsetzenVonPlatz;
  aktiverUmsetzenNachPlatz = umsetzenNachPlatz;
  aktiverUmsetzenTauscht = lagerplatzBelegt(aktiverUmsetzenNachPlatz);
  umsetzenStatus = UMSETZEN_FAHRE_QUELLPLATZ;
  aktualisiereDisplay();

  if (aktiverUmsetzenTauscht) {
    Serial.print(F("Tauschen "));
  }
  else {
    Serial.print(F("Umsetzen "));
  }
  Serial.print(aktiverUmsetzenVonPlatz);
  Serial.print(F(" nach "));
  Serial.println(aktiverUmsetzenNachPlatz);

  ZielPos quelle = lagerplatzPos(aktiverUmsetzenVonPlatz);
  turm.setArbeitsposition(quelle.turmBasis);
  wagen.setArbeitsposition(quelle.wagenPos);
}

void bearbeiteUmsetzen() {
  ZielPos quelle = lagerplatzPos(aktiverUmsetzenVonPlatz);
  ZielPos ziel = lagerplatzPos(aktiverUmsetzenNachPlatz);

  if (umsetzenStatus == UMSETZEN_FAHRE_QUELLPLATZ) {
    if (turm.istFertig() && wagen.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_ZUM_QUELLPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_ZUM_QUELLPLATZ) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(quelle.turmBasis + HUB_IMPULSE);
      umsetzenStatus = UMSETZEN_HEBE_VOM_QUELLPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_HEBE_VOM_QUELLPLATZ) {
    if (turm.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_VOM_QUELLPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_VOM_QUELLPLATZ) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      if (aktiverUmsetzenTauscht) {
        turm.setArbeitsposition(UMSETZPLATZ.turmBasis + HUB_IMPULSE);
        wagen.setArbeitsposition(UMSETZPLATZ.wagenPos);
        umsetzenStatus = UMSETZEN_FAHRE_UMSETZPLATZ_MIT_QUELLE;
      }
      else {
        turm.setArbeitsposition(ziel.turmBasis + HUB_IMPULSE);
        wagen.setArbeitsposition(ziel.wagenPos);
        umsetzenStatus = UMSETZEN_FAHRE_ZIELPLATZ;
      }
    }
  }
  else if (umsetzenStatus == UMSETZEN_FAHRE_UMSETZPLATZ_MIT_QUELLE) {
    if (turm.istFertig() && wagen.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_ZUM_UMSETZPLATZ_QUELLE;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_ZUM_UMSETZPLATZ_QUELLE) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(UMSETZPLATZ.turmBasis);
      umsetzenStatus = UMSETZEN_SENKE_AM_UMSETZPLATZ_QUELLE;
    }
  }
  else if (umsetzenStatus == UMSETZEN_SENKE_AM_UMSETZPLATZ_QUELLE) {
    if (turm.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_VOM_UMSETZPLATZ_QUELLE;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_VOM_UMSETZPLATZ_QUELLE) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turm.setArbeitsposition(ziel.turmBasis);
      wagen.setArbeitsposition(ziel.wagenPos);
      umsetzenStatus = UMSETZEN_FAHRE_ZIEL_ZUM_TAUSCHEN;
    }
  }
  else if (umsetzenStatus == UMSETZEN_FAHRE_ZIEL_ZUM_TAUSCHEN) {
    if (turm.istFertig() && wagen.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_ZUM_ZIEL_ZUM_TAUSCHEN;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_ZUM_ZIEL_ZUM_TAUSCHEN) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(ziel.turmBasis + HUB_IMPULSE);
      umsetzenStatus = UMSETZEN_HEBE_VOM_ZIEL_ZUM_TAUSCHEN;
    }
  }
  else if (umsetzenStatus == UMSETZEN_HEBE_VOM_ZIEL_ZUM_TAUSCHEN) {
    if (turm.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_VOM_ZIEL_ZUM_TAUSCHEN;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_VOM_ZIEL_ZUM_TAUSCHEN) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turm.setArbeitsposition(quelle.turmBasis + HUB_IMPULSE);
      wagen.setArbeitsposition(quelle.wagenPos);
      umsetzenStatus = UMSETZEN_FAHRE_QUELLE_ZUM_ABLEGEN;
    }
  }
  else if (umsetzenStatus == UMSETZEN_FAHRE_QUELLE_ZUM_ABLEGEN) {
    if (turm.istFertig() && wagen.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_ZUR_QUELLE_ZUM_ABLEGEN;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_ZUR_QUELLE_ZUM_ABLEGEN) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(quelle.turmBasis);
      umsetzenStatus = UMSETZEN_SENKE_AN_QUELLE;
    }
  }
  else if (umsetzenStatus == UMSETZEN_SENKE_AN_QUELLE) {
    if (turm.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_VON_QUELLE;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_VON_QUELLE) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turm.setArbeitsposition(UMSETZPLATZ.turmBasis);
      wagen.setArbeitsposition(UMSETZPLATZ.wagenPos);
      umsetzenStatus = UMSETZEN_FAHRE_UMSETZPLATZ_ZUM_AUFNEHMEN;
    }
  }
  else if (umsetzenStatus == UMSETZEN_FAHRE_UMSETZPLATZ_ZUM_AUFNEHMEN) {
    if (turm.istFertig() && wagen.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_ZUM_UMSETZPLATZ_AUFNEHMEN;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_ZUM_UMSETZPLATZ_AUFNEHMEN) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(UMSETZPLATZ.turmBasis + HUB_IMPULSE);
      umsetzenStatus = UMSETZEN_HEBE_VOM_UMSETZPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_HEBE_VOM_UMSETZPLATZ) {
    if (turm.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_VOM_UMSETZPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_VOM_UMSETZPLATZ) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turm.setArbeitsposition(ziel.turmBasis + HUB_IMPULSE);
      wagen.setArbeitsposition(ziel.wagenPos);
      umsetzenStatus = UMSETZEN_FAHRE_ZIELPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_FAHRE_ZIELPLATZ) {
    if (turm.istFertig() && wagen.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_ZUM_ZIELPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_ZUM_ZIELPLATZ) {
    fahreGabelVor();
    if (gabelIstVorne()) {
      turm.setArbeitsposition(ziel.turmBasis);
      umsetzenStatus = UMSETZEN_SENKE_AM_ZIELPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_SENKE_AM_ZIELPLATZ) {
    if (turm.istFertig()) {
      umsetzenStatus = UMSETZEN_GABEL_VOM_ZIELPLATZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_GABEL_VOM_ZIELPLATZ) {
    fahreGabelZurueck();
    if (gabelIstHinten()) {
      turmReferenziert = false;
      wagenReferenziert = false;
      turm.referenzStart();
      wagen.referenzStart();
      umsetzenStatus = UMSETZEN_FAHRE_REFERENZ;
    }
  }
  else if (umsetzenStatus == UMSETZEN_FAHRE_REFERENZ) {
    if (!turmReferenziert && turm.referenzErreicht()) {
      turm.stop();
      turm.referenzSetzen();
      turmReferenziert = true;
    }

    if (!wagenReferenziert && wagen.referenzErreicht()) {
      wagen.stop();
      wagen.referenzSetzen();
      wagenReferenziert = true;
    }

    if (turmReferenziert && wagenReferenziert) {
      umsetzenStatus = UMSETZEN_AUS;
      if (aktiverUmsetzenTauscht) {
        tauscheWare(aktiverUmsetzenVonPlatz, aktiverUmsetzenNachPlatz);
      }
      else {
        verschiebeWare(aktiverUmsetzenVonPlatz, aktiverUmsetzenNachPlatz);
      }
      umsetzenVonPlatz = 0;
      umsetzenNachPlatz = 0;
      aktiverUmsetzenVonPlatz = 0;
      aktiverUmsetzenNachPlatz = 0;
      aktiverUmsetzenTauscht = false;
      Serial.println(F("Umsetzen beendet"));
      gibBerichtAus();
      aktualisiereDisplay();
    }
  }
}

bool referenzfahrtAktiv() {
  return referenzStatus != REFERENZ_AUS;
}

bool referenzpositionErreicht() {
  return gabelIstHinten() &&
         turm.referenzErreicht() &&
         wagen.referenzErreicht();
}

void pruefeReferenzpositionBeimStart() {
  if (!referenzpositionErreicht()) {
    return;
  }

  stopGabel();
  turm.stop();
  wagen.stop();
  turm.referenzSetzen();
  wagen.referenzSetzen();
  referenziert = true;
  referenzOkHinweisAktiv = true;

  Serial.println(F("Referenzposition beim Start erkannt"));
}

void starteReferenzfahrt() {
  wagen.stop();
  turm.stop();
  stopGabel();
  referenziert = false;
  referenzOkHinweisAktiv = false;
  einlagernStatus = EINLAGERN_AUS;
  auslagernStatus = AUSLAGERN_AUS;
  umsetzenStatus = UMSETZEN_AUS;
  aktiverEinlagernPlatz = 0;
  aktiverAuslagernPlatz = 0;
  aktiverUmsetzenVonPlatz = 0;
  aktiverUmsetzenNachPlatz = 0;
  aktiverUmsetzenTauscht = false;
  ausgewaehlterLagerplatz = 0;
  umsetzenVonPlatz = 0;
  umsetzenNachPlatz = 0;
  automatikAuswahl = AUTOMATIK_KEINE;
  automatikAnzeigePlatz = 0;

  // Merker fuer diesen Referenzlauf: Der Endschalter kann schon erreicht sein,
  // aber jede Achse soll erst nach Start der Referenzfahrt auf 0 gesetzt werden.
  turmReferenziert = false;
  wagenReferenziert = false;
  referenzDisplayInitialisiert = false;

  referenzStatus = REFERENZ_GABEL;
}

void stoppeReferenzfahrt() {
  stopGabel();
  turm.stop();
  wagen.stop();
  referenzStatus = REFERENZ_AUS;
  turmReferenziert = false;
  wagenReferenziert = false;
  referenzDisplayInitialisiert = false;
  aktualisiereDisplay();
}

void bearbeiteReferenzfahrt() {
  if (referenzStatus == REFERENZ_GABEL) {
    // Die Gabel ist kein Antriebsmodul, deshalb wird sie hier separat
    // bis zum hinteren Endschalter gefahren.
    fahreGabelZurueck();

    if (gabelIstHinten()) {
      stopGabel();

      // Fuer Turm und Wagen ist die Referenzfahrt dieselbe Bewegung wie
      // bei den Tasten 4 und 7: nach links bis zum Endschalter.
      turm.referenzStart();
      wagen.referenzStart();

      referenzStatus = REFERENZ_ACHSEN;
    }
  }
  else if (referenzStatus == REFERENZ_ACHSEN) {
    // Das Antriebsmodul stoppt links am Endschalter auch in update().
    // Hier wird zusaetzlich festgehalten, wann beide Achsen dieses Ziel
    // innerhalb der gemeinsamen Referenzfahrt erreicht haben.
    if (!turmReferenziert && turm.referenzErreicht()) {
      turm.stop();
      turm.referenzSetzen();
      turmReferenziert = true;
    }

    if (!wagenReferenziert && wagen.referenzErreicht()) {
      wagen.stop();
      wagen.referenzSetzen();
      wagenReferenziert = true;
    }

    if (turmReferenziert && wagenReferenziert) {
      referenziert = true;
      referenzOkHinweisAktiv = true;
      referenzStatus = REFERENZ_AUS;
      referenzDisplayInitialisiert = false;
      Serial.println(F("Referenzfahrt beendet"));
      aktualisiereDisplay();
      return;
    }
  }

  displayReferenzfahrtStatus();
}
