/**
 * @file HardwareControl.h
 * @brief Headerbestand voor hardware-aansturing zoals knoppen, nachtmodus, lichtstabiliteit en ventilatoren (ESP32-S3).
 */

#ifndef HARDWARE_CONTROL_H
#define HARDWARE_CONTROL_H

#include <Arduino.h>



// Functie prototypes
void setupFans();
void checkFlashButton();
void handleWifiStatusLed();
void handleNightMode(float currentLux);
void checkLuxStability(float currentLux);
void calculateRPM();
void updateFanSpeeds(float targetVal);

// NIEUW
void toggleFanAlerts(bool enable);
bool getFanAlertsStatus();
extern bool fanAlertsEnabled;

#endif // HARDWARE_CONTROL_H