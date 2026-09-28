/**
 * @file Logger.cpp
 * @brief Bevat de instantie van de PicoSyslog Logger en de initialisatielogica.
 */

#include "Logger.h"

// Maak het object aan en geef direct de app-naam mee
PicoSyslog::Logger syslog("De kleine kas");

void setupLogger() {
  // Wijs direct de server en poort toe
  syslog.server = "192.168.1.110";
  syslog.port = 6516; 

  Serial.println(F("[LOGGER] Syslog server ingesteld op 192.168.1.110:6516"));
}