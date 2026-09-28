#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "config.h"

// Log-functies (Serial + Telnet)
void logPrint(const String &msg);
void logPrintln(const String &msg);
void logPrint(const __FlashStringHelper *msg);
void logPrintln(const __FlashStringHelper *msg);

void setupNetwork();
void handleNetwork();

#endif