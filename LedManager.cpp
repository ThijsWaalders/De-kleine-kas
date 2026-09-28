/**
 * @file LedManager.cpp
 * @brief Implementatie van de LedManager class voor NeoPixel aansturing en dynamische status-feedback.
 */

#include "LedManager.h"
#include "ClimateLogic.h"
#include <WiFi.h>

// Externe variabelen uit je project om de status te bepalen
extern bool moldRisk;
extern float kasSmoothedHum;
extern int fanIntSpeed;
extern int fanExt1Speed;
extern int fanExt2Speed;
extern HouseVentState houseAdvice;
extern bool isTestModeActive;

// Daadwerkelijke globale instantie
LedManager statusLed;

/**
 * @brief Constructor: Configureert aantal pixels, pin en kleurvolgorde.
 */
LedManager::LedManager() 
  : pixels(NUMPIXELS, PIN_NEOPIXEL, NEO_RGB + NEO_KHZ800) {}

/**
 * @brief Start de NeoPixel hardware en zet de helderheid uit config.h.
 */
void LedManager::begin() {
  pixels.begin();
  pixels.setBrightness(LED_BRIGHTNESS);
  pixels.clear();
  pixels.show();
}

/**
 * @brief Past direct de RGB-kleur aan van de NeoPixel.
 */
void LedManager::setColor(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

/**
 * @brief Maakt de LED volledig donker.
 */
void LedManager::clear() {
  pixels.clear();
  pixels.show();
}

/**
 * @brief Beheert het dynamische LED-gedrag op basis van de kas- en klimaatstatus.
 */
void LedManager::update() {
  unsigned long currentMillis = millis();

  // 0. WiFi niet verbonden -> Snel blauw knipperen
  if (WiFi.status() != WL_CONNECTED) {
    if (currentMillis - lastLedUpdate >= 300) {
      lastLedUpdate = currentMillis;
      static bool state = false;
      state = !state;
      if (state) setColor(0, 0, 150);
      else clear();
    }
    return;
  }

  // Opstartfase (eerste 3 minuten) -> Kort paars ademen / rustig aan
  if (currentMillis < 180000) {
    if (currentMillis - lastLedUpdate >= 40) {
      lastLedUpdate = currentMillis;
      if (breathingUp) {
        breathingValue += 2;
        if (breathingValue >= 50) breathingUp = false;
      } else {
        breathingValue -= 2;
        if (breathingValue <= 2) breathingUp = true;
      }
      setColor(30, 0, 50); // Zacht paars tijdens boot
    }
    return;
  }

  // 1. KRITIEK / ACTIE VEREIST -> Rood, snel knipperen (Schimmelgevaar of raam open zetten)
  if (moldRisk || kasSmoothedHum > 85.0 || (houseAdvice == HOUSE_VENTILATE && moldRisk)) {
    if (currentMillis - lastLedUpdate >= 250) { // Snelle flits
      lastLedUpdate = currentMillis;
      static bool state = false;
      state = !state;
      if (state) {
        setColor(180, 0, 0); // Feller rood, net zichtbaar in het donker
      } else {
        clear();
      }
    }
  }
  // 2. ZWAAR BIJSTUREN (Moeite / ventilatoren draaien voluit) -> Oranje, matig snel knipperen
  else if (fanIntSpeed > 180 || fanExt1Speed > 180 || fanExt2Speed > 180) {
    if (currentMillis - lastLedUpdate >= 500) {
      lastLedUpdate = currentMillis;
      static bool state = false;
      state = !state;
      if (state) {
        setColor(140, 60, 0); // Zacht oranje
      } else {
        clear();
      }
    }
  }
  // 3. NORMAAL REGELEN -> Heel zacht blauw ademen (langzame fade)
  else if (fanIntSpeed > 20 || fanExt1Speed > 20 || fanExt2Speed > 20 || isTestModeActive) {
    if (currentMillis - lastLedUpdate >= 40) { // Vloeiende interval
      lastLedUpdate = currentMillis;
      
      if (breathingUp) {
        breathingValue += 2;
        if (breathingValue >= 40) breathingUp = false; // Maximaal heel zacht (40 van 255)
      } else {
        breathingValue -= 2;
        if (breathingValue <= 2) breathingUp = true;
      }
      setColor(0, 0, breathingValue);
    }
  }
  // 4. ALLES OKÉ -> LED VOLLEDIG UIT
  else {
    clear();
  }
}

// /**
//  * @file LedManager.cpp
//  * @brief Implementatie van de LedManager class voor NeoPixel aansturing.
//  */

// #include "LedManager.h"

// // Daadwerkelijke globale instantie (zonder haakjes, zodat de compiler het als object ziet)
// LedManager statusLed;

// /**
//  * @brief Constructor: Configureert aantal pixels, pin en kleurvolgorde (NEO_RGB voor de meeste ESP32-S3 boards).
//  */
// LedManager::LedManager() 
//   : pixels(NUMPIXELS, PIN_NEOPIXEL, NEO_RGB + NEO_KHZ800) {}

// /**
//  * @brief Start de NeoPixel hardware en zet de helderheid uit config.h.
//  */
// void LedManager::begin() {
//   pixels.begin();
//   pixels.setBrightness(LED_BRIGHTNESS);
//   pixels.clear();
//   pixels.show();
// }

// /**
//  * @brief Past direct de RGB-kleur aan van de NeoPixel.
//  */
// void LedManager::setColor(uint8_t r, uint8_t g, uint8_t b) {
//   pixels.setPixelColor(0, pixels.Color(r, g, b));
//   pixels.show();
// }

// /**
//  * @brief Maakt de LED volledig donker.
//  */
// void LedManager::clear() {
//   pixels.clear();
//   pixels.show();
// }