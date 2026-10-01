/**
 * @file LedManager.cpp
 * @brief Implementatie van de LedManager class met flexibele per-status instellingen uit Config.h.
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
  // Bepaal de effectieve helderheid: als het donker is (lampen uit), max helderheid beperken tot 5
  uint8_t targetBrightness = setting.brightness;
  if (currentLuxValue <= 1.0) {
    targetBrightness = min((int)targetBrightness, 2); // Fluisterstil en zacht in het donker
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
      // Schaal de ademhaling mee met de aangepaste helderheid
      uint8_t dynBrightness = 2 + (breathe * (targetBrightness - 2));
      
      setColor(setting.r, setting.g, setting.b, dynBrightness);
      break;
    }
  }
}
// // Interne helper om de configuratie-instelling toe te passen
// void LedManager::applySetting(const LedConfig::LedSetting& setting, unsigned long currentMillis) {
//   switch (setting.animation) {
//     case LedConfig::OFF:
//       clear();
//       break;

//     case LedConfig::SOLID:
//       setColor(setting.r, setting.g, setting.b, setting.brightness);
//       break;

//     case LedConfig::BLINK:
//       if (currentMillis - lastLedUpdate >= setting.speedMs) {
//         lastLedUpdate = currentMillis;
//         static bool state = false;
//         state = !state;
//         if (state) {
//           setColor(setting.r, setting.g, setting.b, setting.brightness);
//         } else {
//           clear();
//         }
//       }
//       break;

//     case LedConfig::BREATHE: {
//       float progress = (currentMillis % setting.speedMs) / (float)setting.speedMs;
//       float breathe = (sin(progress * 2.0 * PI) + 1.0) / 2.0; 
//       uint8_t dynBrightness = 2 + (breathe * (setting.brightness - 2));
      
//       setColor(setting.r, setting.g, setting.b, dynBrightness);
//       break;
//     }
//   }
// }

/**
 * @brief Beheert het dynamische LED-gedrag op basis van prioriteiten en Config.h instellingen.
 */
void LedManager::update() {
  unsigned long currentMillis = millis();

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
//  * @brief Implementatie van de LedManager class voor NeoPixel aansturing en dynamische status-feedback.
//  */

// #include "LedManager.h"
// #include "ClimateLogic.h"
// #include <WiFi.h>

// // Externe variabelen uit Config.h om de status te bepalen
// extern bool hasWeatherAlarm;          
// extern HouseVentState houseAdvice;    
// extern int fanIntSpeed;               
// extern int fanExt1Speed;              
// extern int fanExt2Speed;              
// extern bool apModeActive;             

// // Daadwerkelijke globale instantie
// LedManager statusLed;

// LedManager::LedManager() 
//   : pixels(NUMPIXELS, PIN_NEOPIXEL, NEO_RGB + NEO_KHZ800) {}

// void LedManager::begin() {
//   pixels.begin();
//   pixels.setBrightness(LedConfig::BRIGHTNESS_NORMAL);
//   pixels.clear();
//   pixels.show();
// }

// void LedManager::setColor(uint8_t r, uint8_t g, uint8_t b) {
//   pixels.setPixelColor(0, pixels.Color(r, g, b));
//   pixels.show();
// }

// void LedManager::clear() {
//   pixels.clear();
//   pixels.show();
// }

// /**
//  * @brief Beheert het dynamische LED-gedrag op basis van prioriteiten:
//  *        1. Netwerk / AP (Blauw knipperen)
//  *        2. Buiten Weeralarm (Zacht geel ademen)
//  *        3. Woning Advies / Ventileren of Sluiten (Oranje)
//  *        4. Systeem regelt actief / ventilatoren draaien (Groen)
//  *        5. Alles rustig / Niks te doen (Uit)
//  */
// void LedManager::update() {
//   unsigned long currentMillis = millis();

//   // --- PRIORITEIT 1: Netwerk / Wi-Fi kwijt of in AP-modus ---
//   if (WiFi.status() != WL_CONNECTED) {
//     if (currentMillis - lastLedUpdate >= LedConfig::BLINK_FAST_MS) {
//       lastLedUpdate = currentMillis;
//       static bool state = false;
//       state = !state;
//       if (state) {
//         setColor(LedConfig::COLOR_WIFI_NET.r, LedConfig::COLOR_WIFI_NET.g, LedConfig::COLOR_WIFI_NET.b);
//       } else {
//         clear();
//       }
//     }
//     return;
//   }

//   // --- PRIORITEIT 2: Buiten Weer Alarm (Zacht geel ademen) ---
//   if (hasWeatherAlarm) {
//     float progress = (currentMillis % LedConfig::BREATHE_CYCLE_MS) / (float)LedConfig::BREATHE_CYCLE_MS;
//     float breathe = (sin(progress * 2.0 * PI) + 1.0) / 2.0; 
//     uint8_t dynBrightness = 2 + (breathe * (LedConfig::BRIGHTNESS_ALARM - 2));

//     setColor(
//       (LedConfig::COLOR_WEATHER_WARN.r * dynBrightness) / 255,
//       (LedConfig::COLOR_WEATHER_WARN.g * dynBrightness) / 255,
//       (LedConfig::COLOR_WEATHER_WARN.b * dynBrightness) / 255
//     );
//     return;
//   }

//   // --- PRIORITEIT 3: Woning Advies / Ventileren of Sluiten (Oranje) ---
//   // Triggert zodra het huisadvies op ventileren staat (of pas dit aan als je ook HOUSE_CLOSED wilt meewegen)
//   if (houseAdvice == HOUSE_VENTILATE) { 
//     setColor(LedConfig::COLOR_HOUSE_ADVICE.r, LedConfig::COLOR_HOUSE_ADVICE.g, LedConfig::COLOR_HOUSE_ADVICE.b);
//     return;
//   }

//   // --- PRIORITEIT 4: Systeem regelt actief (Groen) ---
//   // Als een van de ventilatoren actief draait
//   bool systemActive = (fanIntSpeed > 0 || fanExt1Speed > 0 || fanExt2Speed > 0);
//   if (systemActive) {
//     setColor(LedConfig::COLOR_SYSTEM_ACTIVE.r, LedConfig::COLOR_SYSTEM_ACTIVE.g, LedConfig::COLOR_SYSTEM_ACTIVE.b);
//     return;
//   }

//   // --- PRIORITEIT 5: Alles is stabiel en rustig -> LED UIT ---
//   clear();
// }

// // /**
// //  * @file LedManager.cpp
// //  * @brief Implementatie van de LedManager class voor NeoPixel aansturing en dynamische status-feedback.
// //  */

// // #include "LedManager.h"
// // #include "ClimateLogic.h"
// // #include <WiFi.h>

// // // Externe variabelen uit je project om de status te bepalen
// // extern bool moldRisk;
// // extern float kasSmoothedHum;
// // extern int fanIntSpeed;
// // extern int fanExt1Speed;
// // extern int fanExt2Speed;
// // extern HouseVentState houseAdvice;
// // extern bool isTestModeActive;

// // // Daadwerkelijke globale instantie
// // LedManager statusLed;

// // /**
// //  * @brief Constructor: Configureert aantal pixels, pin en kleurvolgorde.
// //  */
// // LedManager::LedManager() 
// //   : pixels(NUMPIXELS, PIN_NEOPIXEL, NEO_RGB + NEO_KHZ800) {}

// // /**
// //  * @brief Start de NeoPixel hardware en zet de helderheid uit config.h.
// //  */
// // void LedManager::begin() {
// //   pixels.begin();
// //   pixels.setBrightness(LED_BRIGHTNESS);
// //   pixels.clear();
// //   pixels.show();
// // }

// // /**
// //  * @brief Past direct de RGB-kleur aan van de NeoPixel.
// //  */
// // void LedManager::setColor(uint8_t r, uint8_t g, uint8_t b) {
// //   pixels.setPixelColor(0, pixels.Color(r, g, b));
// //   pixels.show();
// // }

// // /**
// //  * @brief Maakt de LED volledig donker.
// //  */
// // void LedManager::clear() {
// //   pixels.clear();
// //   pixels.show();
// // }

// // /**
// //  * @brief Beheert het dynamische LED-gedrag op basis van de kas- en klimaatstatus.
// //  */
// // void LedManager::update() {
// //   unsigned long currentMillis = millis();

// //   // 0. WiFi niet verbonden -> Snel blauw knipperen (Handig om te zien dat het netwerk weg is)
// //   if (WiFi.status() != WL_CONNECTED) {
// //     if (currentMillis - lastLedUpdate >= 300) {
// //       lastLedUpdate = currentMillis;
// //       static bool state = false;
// //       state = !state;
// //       if (state) setColor(0, 0, 150);
// //       else clear();
// //     }
// //     return;
// //   }

// //   // 1. KRITIEK / ACTIE VEREIST DOOR JOU (Schimmelgevaar / raam open zetten) -> Rood knipperen
// //   if (moldRisk || kasSmoothedHum > 85.0 || (houseAdvice == HOUSE_VENTILATE && moldRisk)) {
// //     if (currentMillis - lastLedUpdate >= 250) { // Snelle waarschuwingsflits
// //       lastLedUpdate = currentMillis;
// //       static bool state = false;
// //       state = !state;
// //       if (state) {
// //         setColor(180, 0, 0); // Duidelijk rood
// //       } else {
// //         clear();
// //       }
// //     }
// //   }
// //   // 2. AL HET ANDERE (Bootfase, normaal regelen, ventilatoren draaien, testmodus) -> VOLLEDIG UIT
// //   else {
// //     clear();
// //   }
// // }
