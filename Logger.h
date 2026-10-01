#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <PicoSyslog.h>

extern PicoSyslog::Logger syslog;
extern WiFiClient telnetClient; // 👈 Koppel de Telnet-client hier

void setupLogger();

// --- 1. ALLEEN NAAR SERIAL ---
template<typename T>
inline void logToSerial(T message) {
  Serial.println(message);
}

// --- 2. NAAR SERIAL, SYSLOG ÉN TELNET (INDIEN ACTIEF) ---
template<typename T>
inline void logToSyslogAndSerial(T message) {
  Serial.println(message); // Altijd lokaal printen voor Serial Monitor

  // Naar Telnet sturen als je bent ingelogd via de draadloze terminal
  if (telnetClient && telnetClient.connected()) {
    telnetClient.println(message);
  }

  // Naar netwerk syslog sturen indien verbonden
  if (WiFi.status() == WL_CONNECTED) {
    syslog.println(String(message)); 
  }
}

// --- 3. FORMATTERINGSFUNCTIES (Printf) ---
template <typename... Args>
void logToSyslogAndSerialPrintf(const char* format, Args... args) {
  char buf[256];
  snprintf(buf, sizeof(buf), format, args...);
  logToSyslogAndSerial(String(buf));
}

template <typename... Args>
void logToSerialPrintf(const char* format, Args... args) {
  char buf[256];
  snprintf(buf, sizeof(buf), format, args...);
  logToSerial(String(buf));
}

#endif