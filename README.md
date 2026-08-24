# fischertechnik Hochregallager mit ftDuino

Programme, Dokumentationen und weitere Unterlagen zu meinem fischertechnik Hochregallager mit ftDuino-Steuerung.

## Projektbeschreibung
Die hier vorgestellte Variante des fischertechnik Hochregallagers wird mit einem **ftDuino-Controller** gesteuert. An dessen I²C-Port werden eine **4×4-Matrixtastatur** und ein **0,96"-OLED-Display** betrieben.

Gegenüber dem Original ergeben sich dadurch folgende Vorteile:

* kostengünstige Steuerungshardware
* Programmierung auf Basis von C++ bzw. C
* menügeführte Bedienung
* vom Computer unabhängiger Betrieb

## Hardware
Der mechanische Aufbau und die grundlegende Verdrahtung des Hochregallagers erfolgen entsprechend der fischertechnik Bauanleitung „ROBO TX Automation Robots“.

Der folgende Schaltplan zeigt die im YouTube-Video verwendete Hardware-Erweiterung.

![Hardware-Erweiterung](https://raw.githubusercontent.com/HR-Projekte/ft-Hochregallager-und-ft-3-Achs-Roboter/main/Bilder/Hardware-Erweiterung.jpg)

Eine billigere alternative I2C-Konfiguration gibt's hier:
[I2C-Alternative](https://github.com/HR-Projekte/ft-Hochregallager-und-ft-3-Achs-Roboter/blob/main/Bilder/I2C-alternativ.jpg)

## Software
Das Steuerungsprogramm für das Hochregallager ist als Arduino-Sketch abgelegt.
[Arduino-Programm Hochregallager](Programme/Hochregallager/)

## Dokumentation
[Die Menüstruktur](https://github.com/HR-Projekte/ft-Hochregallager-und-ft-3-Achs-Roboter/blob/main/Dokumentation/Menue-Ablauf.pdf)

## Videos
