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
extern float espInternalTemp;         
extern float currentLuxValue;         // Zorg dat deze globaal beschikbaar is (indien nodig)

// Daadwerkelijke globale instantie met pin en aantal pixels volgens Config.h
LedManager statusLed(PIN_NEOPIXEL, NUMPIXELS);

LedManager::LedManager(uint8_t pin, uint8_t numPixels) 
  : pixels(numPixels, pin, NEO_RGB + NEO_KHZ800), lastLedUpdate(0) {}

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

// Interne helper om de configuratie-instelling toe te passen
void LedManager::applySetting(const LedConfig::LedSetting& setting, unsigned long currentMillis) {
  uint8_t targetBrightness = setting.brightness;
  // Veiligheidshalve controleren op currentLuxValue indien gedefinieerd in project
  // if (currentLuxValue <= 1.0) {
  //   targetBrightness = min((int)targetBrightness, 2); 
  // }

  switch (setting.mode) {
    case OFF:
      clear();
      break;

    case SOLID:
      setColor(setting.red, setting.green, setting.blue, targetBrightness);
      break;

    case BLINK:
      if (currentMillis - lastLedUpdate >= setting.interval) {
        lastLedUpdate = currentMillis;
        static bool state = false;
        state = !state;
        if (state) {
          setColor(setting.red, setting.green, setting.blue, targetBrightness);
        } else {
          clear();
        }
      }
      break;

    case BREATHE: {
      float progress = (currentMillis % setting.interval) / (float)setting.interval;
      float breathe = (sin(progress * 2.0 * PI) + 1.0) / 2.0; 
      uint8_t dynBrightness = 2 + (breathe * (targetBrightness - 2));
      
      setColor(setting.red, setting.green, setting.blue, dynBrightness);
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
    static unsigned long lastChipAlertFlash = 0;
    static bool chipAlertState = false;
    if (currentMillis - lastChipAlertFlash >= 200) {
      lastChipAlertFlash = currentMillis;
      chipAlertState = !chipAlertState;
    }
    if (chipAlertState) {
      setColor(255, 0, 0, 255); 
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
