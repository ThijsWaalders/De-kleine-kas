/**
 * @file Display.h
 * @brief Headerbestand voor OLED-schermfuncties, iconen en animaties (ESP32-S3).
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>
#include "Config.h"
#include <Wire.h>
#include <SSD1306Wire.h>

// Externe referentie naar het OLED-scherm object
extern SSD1306Wire display;

// Functie prototypes
void drawBootScreen(String dots);
void handleNightMode(float currentLux);
void handleDisplayPower(); // <--- Deze even toegevoegd!
void renderDisplay();

#endif // DISPLAY_H
