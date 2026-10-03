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
