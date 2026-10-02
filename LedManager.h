#ifndef LED_MANAGER_H
#define LED_MANAGER_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "Config.h"

class LedManager {
private:
  Adafruit_NeoPixel pixels;
  unsigned long lastLedUpdate;

public:
  LedManager(uint8_t pin, uint8_t numPixels);
  void begin();
  void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 5);
  void clear();
  void applySetting(const LedConfig::LedSetting& setting, unsigned long currentMillis);
  void update();
};

extern LedManager statusLed;

#endif

// /**
//  * @file LedManager.h
//  * @brief Beheert de ingebouwde NeoPixel LED van de ESP32-S3-Zero met flexibele per-status instellingen.
//  */

// #ifndef LED_MANAGER_H
// #define LED_MANAGER_H

// #include <Arduino.h>
// #include <Adafruit_NeoPixel.h>
// #include "Config.h"

// class LedManager {
// private:
//   Adafruit_NeoPixel pixels;
//   unsigned long lastLedUpdate = 0;

//   /**
//    * @brief Interne helper die de configuratie-instelling toepast.
//    */
//   void applySetting(const LedConfig::LedSetting& setting, unsigned long currentMillis);

// public:
//   LedManager();
//   void begin();
//   void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 30);
//   void clear();
//   void update();
// };

// // Globale instantie beschikbaar voor het hele project
// extern LedManager statusLed;

// #endif // LED_MANAGER_H
