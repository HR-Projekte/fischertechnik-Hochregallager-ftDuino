/*
===============================================================
  Projekt:    fischertechnik Hochregallager mit ftDuino
  Datei:      Antriebsmodul.cpp
  Autor:      HR

  GitHub:     HR-Projekte
===============================================================
*/
#include "Antriebsmodul.h"

Antriebsmodul::Antriebsmodul(
  int prt,
  int max,
  int speed
)
  : port(prt),
    count_max(max),
    geschwindigkeit(speed),
    bewegung(Ftduino::BRAKE),
    countAbsolut(0),
    zielPosition(0),
    lastCounter(0),
    achsfahrtAktiv(false) {}
    
void Antriebsmodul::init() {
  ftduino.counter_set_mode(port, Ftduino::C_EDGE_ANY);
  ftduino.input_set_mode(port, Ftduino::SWITCH);
  lastCounter = ftduino.counter_get(port);
} 

void Antriebsmodul::setBewegung(int richtung) {
  if (richtung == Ftduino::RIGHT && countAbsolut >= count_max) {
    richtung = Ftduino::BRAKE;
  }
  if (richtung == Ftduino::LEFT && referenzErreicht()) {
    richtung = Ftduino::BRAKE;
  }

  achsfahrtAktiv = false;
  bewegung = richtung;
  ftduino.motor_set(port, bewegung, geschwindigkeit);
}

void Antriebsmodul::setArbeitsposition(int pos) {
  if (pos < 0) {
    pos = 0;
  }
  if (pos > count_max) {
    pos = count_max;
  }

  zielPosition = pos;
  achsfahrtAktiv = true;
}

bool Antriebsmodul::istFertig() {
  return !achsfahrtAktiv;
}

int Antriebsmodul::getPosition() {
  return countAbsolut;
}

int Antriebsmodul::getPort() {
  return port;
}

int Antriebsmodul::getCountMax() {
  return count_max;
}

void Antriebsmodul::referenzStart() {
  setBewegung(Ftduino::LEFT);
}

bool Antriebsmodul::referenzErreicht() {
  return ftduino.input_get(port);
}

void Antriebsmodul::referenzSetzen() {
  countAbsolut = 0;
  ftduino.counter_clear(port);
  lastCounter = 0;
}

void Antriebsmodul::stop() {
  if (bewegung == Ftduino::BRAKE) {
    achsfahrtAktiv = false;
    return;
  }

  ftduino.motor_set(port, Ftduino::BRAKE, geschwindigkeit);
  bewegung = Ftduino::BRAKE;
  achsfahrtAktiv = false;
}

void Antriebsmodul::update() {
  int current = ftduino.counter_get(port);
  int delta = current - lastCounter;
  lastCounter = current;

  if (bewegung == Ftduino::RIGHT) {
    countAbsolut += delta;
  }
  else if (bewegung == Ftduino::LEFT) {
    countAbsolut -= delta;
  }

  if (countAbsolut < 0) {
    countAbsolut = 0;
  }
  if (countAbsolut > count_max) {
    countAbsolut = count_max;
  }

  // Stopp an mechanischen Endlagen
  if (bewegung == Ftduino::LEFT && ftduino.input_get(port)) {
    stop();
    referenzSetzen();
  }

  if (bewegung == Ftduino::RIGHT && countAbsolut >= count_max) {
    stop();
    countAbsolut = count_max;
  }

  // Automatik-Ansteuerung: Ziel bei Erreichen oder Ueberfahren stoppen.
  if (achsfahrtAktiv) {
    if ((bewegung == Ftduino::RIGHT && countAbsolut >= zielPosition) ||
        (bewegung == Ftduino::LEFT && countAbsolut <= zielPosition) ||
        countAbsolut == zielPosition) {
      countAbsolut = zielPosition;
      stop();
    }
    else if (countAbsolut < zielPosition && bewegung != Ftduino::RIGHT) {
      bewegung = Ftduino::RIGHT;
      ftduino.motor_set(port, bewegung, geschwindigkeit);
    }
    else if (countAbsolut > zielPosition && bewegung != Ftduino::LEFT) {
      bewegung = Ftduino::LEFT;
      ftduino.motor_set(port, bewegung, geschwindigkeit);
    }
  }
}
