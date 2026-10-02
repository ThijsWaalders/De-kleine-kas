/**
 * @file LedManager.cpp
 * @brief Implementatie van de LedManager class met flexibele per-status instellingen uit Config.h en hardware-beveiliging.
 */

#include "LedManager.h"
#include "ClimateLogic.h"
#include <WiFi.h>

// Externe variabelen uit je project
extern bool hasWeatherAlarm;          
extern HouseVentState houseAdvice;    
extern int fanIntSpeed;               
extern int fanExt1Speed;              
extern int fanExt2Speed;              
extern float espInternalTemp;         // <-- Toegevoegd voor de temperatuurbeveiliging

// Daadwerkelijke globale instantie
LedManager statusLed;

LedManager::LedManager() 
  : pixels(NUMPIXELS, PIN_NEOPIXEL, NEO_RGB + NEO_KHZ800) {}

void LedManager::begin() {
  pixels.begin();
  pixels.clear();
  pixels.show();
}

// Altijd 4 argumenten (inclusief brightness)
void LedManager::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  pixels.setBrightness(brightness);
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

void LedManager::clear() {
  pixels.clear();
  pixels.show();
}

// Interne helper om de configuratie-instelling toepast, inclusief nacht-dimming op basis van lichtsensor
void LedManager::applySetting(const LedConfig::LedSetting& setting, unsigned long currentMillis) {
  uint8_t targetBrightness = setting.brightness;
  if (currentLuxValue <= 1.0) {
    targetBrightness = min((int)targetBrightness, 2); 
  }

  switch (setting.animation) {
    case LedConfig::OFF:
      clear();
      break;

    case LedConfig::SOLID:
      setColor(setting.r, setting.g, setting.b, targetBrightness);
      break;

    case LedConfig::BLINK:
      if (currentMillis - lastLedUpdate >= setting.speedMs) {
        lastLedUpdate = currentMillis;
        static bool state = false;
        state = !state;
        if (state) {
          setColor(setting.r, setting.g, setting.b, targetBrightness);
        } else {
          clear();
        }
      }
      break;

    case LedConfig::BREATHE: {
      float progress = (currentMillis % setting.speedMs) / (float)setting.speedMs;
      float breathe = (sin(progress * 2.0 * PI) + 1.0) / 2.0; 
      uint8_t dynBrightness = 2 + (breathe * (targetBrightness - 2));
      
      setColor(setting.r, setting.g, setting.b, dynBrightness);
      break;
    }
  }
}

/**
 * @brief Beheert het dynamische LED-gedrag op basis van prioriteiten en Config.h instellingen.
 */
void LedManager::update() {
  unsigned long currentMillis = millis();

  // 0. ABSOLUTE PRIORITEIT: ESP32 Chip Oververhitting / Hardware Gezondheid (> 70°C)
  if (espInternalTemp >= 70.0) {
    // Snel fel rood knipperen (elke 200ms) bij kritieke hitte
    static unsigned long lastChipAlertFlash = 0;
    static bool chipAlertState = false;
    if (currentMillis - lastChipAlertFlash >= 200) {
      lastChipAlertFlash = currentMillis;
      chipAlertState = !chipAlertState;
    }
    if (chipAlertState) {
      setColor(255, 0, 0, 255); // Volle helderheid fel rood
    } else {
      clear();
    }
    return;
  }

  // 1. Wi-Fi niet verbonden
  if (WiFi.status() != WL_CONNECTED) {
    applySetting(LedConfig::WIFI_DISCONNECTED, currentMillis);
    return;
  }

  // 2. Buiten Weer Alarm
  if (hasWeatherAlarm) {
    applySetting(LedConfig::WEATHER_ALARM, currentMillis);
    return;
  }

  // 3. Woning Advies (Ventileren)
  if (houseAdvice == HOUSE_VENTILATE) {
    applySetting(LedConfig::HOUSE_ADVICE, currentMillis);
    return;
  }

  // 4. Systeem regelt actief
  bool systemActive = (fanIntSpeed > 0 || fanExt1Speed > 0 || fanExt2Speed > 0);
  if (systemActive) {
    applySetting(LedConfig::SYSTEM_ACTIVE, currentMillis);
    return;
  }

  // 5. Alles is stabiel en rustig
  applySetting(LedConfig::ALL_OK_IDLE, currentMillis);
}

// /**
//  * @file LedManager.cpp
//  * @brief Implementatie van de LedManager class met flexibele per-status instellingen uit Config.h.
//  */

// #include "LedManager.h"
// #include "ClimateLogic.h"
// #include <WiFi.h>

// // Externe variabelen uit je project
// extern bool hasWeatherAlarm;          
// extern HouseVentState houseAdvice;    
// extern int fanIntSpeed;               
// extern int fanExt1Speed;              
// extern int fanExt2Speed;              

// // Daadwerkelijke globale instantie
// LedManager statusLed;

// LedManager::LedManager() 
//   : pixels(NUMPIXELS, PIN_NEOPIXEL, NEO_RGB + NEO_KHZ800) {}

// void LedManager::begin() {
//   pixels.begin();
//   pixels.clear();
//   pixels.show();
// }

// // Altijd 4 argumenten (inclusief brightness)
// void LedManager::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
//   pixels.setBrightness(brightness);
//   pixels.setPixelColor(0, pixels.Color(r, g, b));
//   pixels.show();
// }

// void LedManager::clear() {
//   pixels.clear();
//   pixels.show();
// }

// // Interne helper om de configuratie-instelling toepast, inclusief nacht-dimming op basis van lichtsensor
// void LedManager::applySetting(const LedConfig::LedSetting& setting, unsigned long currentMillis) {
//   // Bepaal de effectieve helderheid: als het donker is (lampen uit), max helderheid beperken tot 5
//   uint8_t targetBrightness = setting.brightness;
//   if (currentLuxValue <= 1.0) {
//     targetBrightness = min((int)targetBrightness, 2); // Fluisterstil en zacht in het donker
//   }

//   switch (setting.animation) {
//     case LedConfig::OFF:
//       clear();
//       break;

//     case LedConfig::SOLID:
//       setColor(setting.r, setting.g, setting.b, targetBrightness);
//       break;

//     case LedConfig::BLINK:
//       if (currentMillis - lastLedUpdate >= setting.speedMs) {
//         lastLedUpdate = currentMillis;
//         static bool state = false;
//         state = !state;
//         if (state) {
//           setColor(setting.r, setting.g, setting.b, targetBrightness);
//         } else {
//           clear();
//         }
//       }
//       break;

//     case LedConfig::BREATHE: {
//       float progress = (currentMillis % setting.speedMs) / (float)setting.speedMs;
//       float breathe = (sin(progress * 2.0 * PI) + 1.0) / 2.0; 
//       // Schaal de ademhaling mee met de aangepaste helderheid
//       uint8_t dynBrightness = 2 + (breathe * (targetBrightness - 2));
      
//       setColor(setting.r, setting.g, setting.b, dynBrightness);
//       break;
//     }
//   }
// }
// // // Interne helper om de configuratie-instelling toe te passen
// // void LedManager::applySetting(const LedConfig::LedSetting& setting, unsigned long currentMillis) {
// //   switch (setting.animation) {
// //     case LedConfig::OFF:
// //       clear();
// //       break;

// //     case LedConfig::SOLID:
// //       setColor(setting.r, setting.g, setting.b, setting.brightness);
// //       break;

// //     case LedConfig::BLINK:
// //       if (currentMillis - lastLedUpdate >= setting.speedMs) {
// //         lastLedUpdate = currentMillis;
// //         static bool state = false;
// //         state = !state;
// //         if (state) {
// //           setColor(setting.r, setting.g, setting.b, setting.brightness);
// //         } else {
// //           clear();
// //         }
// //       }
// //       break;

// //     case LedConfig::BREATHE: {
// //       float progress = (currentMillis % setting.speedMs) / (float)setting.speedMs;
// //       float breathe = (sin(progress * 2.0 * PI) + 1.0) / 2.0; 
// //       uint8_t dynBrightness = 2 + (breathe * (setting.brightness - 2));
      
// //       setColor(setting.r, setting.g, setting.b, dynBrightness);
// //       break;
// //     }
// //   }
// // }

// /**
//  * @brief Beheert het dynamische LED-gedrag op basis van prioriteiten en Config.h instellingen.
//  */
// void LedManager::update() {
//   unsigned long currentMillis = millis();

//   // 1. Wi-Fi niet verbonden
//   if (WiFi.status() != WL_CONNECTED) {
//     applySetting(LedConfig::WIFI_DISCONNECTED, currentMillis);
//     return;
//   }

//   // 2. Buiten Weer Alarm
//   if (hasWeatherAlarm) {
//     applySetting(LedConfig::WEATHER_ALARM, currentMillis);
//     return;
//   }

//   // 3. Woning Advies (Ventileren)
//   if (houseAdvice == HOUSE_VENTILATE) {
//     applySetting(LedConfig::HOUSE_ADVICE, currentMillis);
//     return;
//   }

//   // 4. Systeem regelt actief
//   bool systemActive = (fanIntSpeed > 0 || fanExt1Speed > 0 || fanExt2Speed > 0);
//   if (systemActive) {
//     applySetting(LedConfig::SYSTEM_ACTIVE, currentMillis);
//     return;
//   }

//   // 5. Alles is stabiel en rustig
//   applySetting(LedConfig::ALL_OK_IDLE, currentMillis);
// }
