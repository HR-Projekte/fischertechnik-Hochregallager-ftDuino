/*
===============================================================
  Projekt:    fischertechnik Hochregallager mit ftDuino
  Datei:     Antriebsmodul.h
  Autor:      HR

  GitHub:     HR-Projekte
===============================================================
*/
#pragma once

#include <Ftduino.h>

class Antriebsmodul {

public:
Antriebsmodul(
  int prt,
  int max,
  int speed
);

void init();
void setBewegung(int richtung);
void setArbeitsposition(int pos);
bool istFertig();
int getPosition();
int getPort();
int getCountMax();
void referenzStart();
bool referenzErreicht();
void referenzSetzen(); 
void stop();
void update();


private:
  int port;
  int count_max;
  int geschwindigkeit;
  int bewegung;
  int countAbsolut;
  int zielPosition;
  int lastCounter;
  bool achsfahrtAktiv;
};
