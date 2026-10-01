/**
 * @file LedManager.h
 * @brief Beheert de ingebouwde NeoPixel LED van de ESP32-S3-Zero met flexibele per-status instellingen.
 */

#ifndef LED_MANAGER_H
#define LED_MANAGER_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "Config.h"

class LedManager {
private:
  Adafruit_NeoPixel pixels;
  unsigned long lastLedUpdate = 0;

  /**
   * @brief Interne helper die de configuratie-instelling toepast.
   */
  void applySetting(const LedConfig::LedSetting& setting, unsigned long currentMillis);

public:
  LedManager();
  void begin();
  void setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness = 30);
  void clear();
  void update();
};

// Globale instantie beschikbaar voor het hele project
extern LedManager statusLed;

#endif // LED_MANAGER_H
// /**
//  * @file LedManager.h
//  * @brief Beheert de ingebouwde NeoPixel LED van de ESP32-S3-Zero.
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
//   int breathingValue = 0;
//   bool breathingUp = true;

// public:
//   /**
//    * @brief Constructor voor LedManager, initialiseert het NeoPixel object op basis van config.h.
//    */
//   LedManager();

//   /**
//    * @brief Initialiseert de NeoPixel hardware en zet de helderheid.
//    */
//   void begin();

//   /**
//    * @brief Stelt de kleur van de status LED in.
//    * @param r Rode waarde (0-255)
//    * @param g Groene waarde (0-255)
//    * @param b Blauwe waarde (0-255)
//    */
//   void setColor(uint8_t r, uint8_t g, uint8_t b);

//   /**
//    * @brief Schakelt de LED uit (maakt hem zwart/leeg).
//    */
//   void clear();

//   /**
//    * @brief Dynamische update functie die de LED aanstuurt op basis van de kasstatus.
//    */
//   void update();
// };

// // Globale instantie beschikbaar voor het hele project
// extern LedManager statusLed;

// #endif // LED_MANAGER_H

// // /**
// //  * @file LedManager.h
// //  * @brief Beheert de ingebouwde NeoPixel LED van de ESP32-S3-Zero.
// //  */

// // #ifndef LED_MANAGER_H
// // #define LED_MANAGER_H

// // #include <Arduino.h>
// // #include <Adafruit_NeoPixel.h>
// // #include "Config.h"

// // class LedManager {
// // private:
// //   Adafruit_NeoPixel pixels;

// // public:
// //   /**
// //    * @brief Constructor voor LedManager, initialiseert het NeoPixel object op basis van config.h.
// //    */
// //   LedManager();

// //   /**
// //    * @brief Initialiseert de NeoPixel hardware en zet de helderheid.
// //    */
// //   void begin();

// //   /**
// //    * @brief Stelt de kleur van de status LED in.
// //    * @param r Rode waarde (0-255)
// //    * @param g Groene waarde (0-255)
// //    * @param b Blauwe waarde (0-255)
// //    */
// //   void setColor(uint8_t r, uint8_t g, uint8_t b);

// //   /**
// //    * @brief Schakelt de LED uit (maakt hem zwart/leeg).
// //    */
// //   void clear();
// // };

// // // Globale instantie beschikbaar voor het hele project
// // extern LedManager statusLed;

// // #endif // LED_MANAGER_H