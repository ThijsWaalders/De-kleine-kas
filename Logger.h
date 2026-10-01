#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <PicoSyslog.h>

extern PicoSyslog::Logger syslog;

void setupLogger();

// --- 1. ALLEEN NAAR SERIAL ---
template<typename T>
inline void logToSerial(T message) {
  Serial.println(message);
}

// --- 2. NAAR SERIAL ÉN SYSLOG ---
template<typename T>
inline void logToSyslogAndSerial(T message) {
  Serial.println(message); // Altijd lokaal printen voor direct inzicht in Serial Monitor
  if (WiFi.status() == WL_CONNECTED) {
    syslog.println(String(message)); // Naar netwerk syslog sturen indien verbonden
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

// #ifndef LOGGER_H
// #define LOGGER_H

// #include <Arduino.h>
// #include <WiFi.h>
// #include <PicoSyslog.h>

// extern PicoSyslog::Logger syslog;

// void setupLogger();

// // --- 1. ALLEEN NAAR SERIAL ---
// template<typename T>
// inline void logToSerial(T message) {
//   Serial.println(message);
// }

// // --- 2. NAAR SERIAL ÉN SYSLOG (Enkelvoudige uitvoering) ---
// template<typename T>
// inline void logToSyslogAndSerial(T message) {
//   if (WiFi.status() == WL_CONNECTED) {
//     // PicoSyslog regelt de netwerk-log én print hem voor je in de console
//     // syslog.println(String(message));
//     logToSyslogAndSerial(String(buf));

//   } else {
//     // Geen Wi-Fi? Dan moeten we hem wél zelf printen
//     Serial.println(message);
//   }
// }
// // Volgende code printe dubbel, bovenstaande print niet meer wanneer wifi verbonden is (dan neemt picosyslog het over?)
// // template<typename T>
// // inline void logToSyslogAndSerial(T message) {
// //   Serial.println(message);
// //   if (WiFi.status() == WL_CONNECTED) {
// //     syslog.println(String(message));
// //   }
// // }

// template <typename... Args>
// void logToSyslogAndSerialPrintf(const char* format, Args... args) {
//   char buf[256];
//   snprintf(buf, sizeof(buf), format, args...);
//   logToSyslogAndSerial(String(buf));
// }

// template <typename... Args>
// void logToSerialPrintf(const char* format, Args... args) {
//   char buf[256];
//   snprintf(buf, sizeof(buf), format, args...);
//   logToSerial(String(buf));
// }

// #endif
// // #ifndef LOGGER_H
// // #define LOGGER_H

// // #include <Arduino.h>
// // #include <WiFi.h>
// // #include <PicoSyslog.h>

// // // Externe referentie naar je syslog object
// // extern PicoSyslog::Logger syslog;

// // // Functie prototype voor het initialiseren van de syslog server
// // void setupLogger();

// // // --- 1. ALLEEN NAAR SERIAL (Voor routine status en simpele taken) ---
// // inline void logToSerial(const char* message) {
// //   Serial.println(message);
// // }

// // template<typename T>
// // inline void logToSerial(T message) {
// //   Serial.println(message);
// // }

// // // --- 2. BELANGRIJK: NAAR SERIAL ÉN SYSLOG (Voor boots, errors en harde statuswissels) ---
// // inline void logToSyslogAndSerial(const char* message) {
// //   Serial.println(message);
// //   if (WiFi.status() == WL_CONNECTED) {
// //     syslog.println(message);
// //   }
// // }

// // template<typename T>
// // inline void logToSyslogAndSerial(T message) {
// //   Serial.println(message);
// //   if (WiFi.status() == WL_CONNECTED) {
// //     syslog.println(String(message));
// //   }
// // }


// // template <typename... Args>
// // void logToSyslogAndSerialPrintf(const char* format, Args... args) {
// //   char buf[256];
// //   snprintf(buf, sizeof(buf), format, args...);
// //   logToSyslogAndSerial(String(buf));
// // }

// // template <typename... Args>
// // void logToSerialPrintf(const char* format, Args... args) {
// //   char buf[256];
// //   snprintf(buf, sizeof(buf), format, args...);
// //   logToSerial(String(buf));
// // }


// // #endif