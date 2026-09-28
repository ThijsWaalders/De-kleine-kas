#ifndef LED_MANAGER_H
#define LED_MANAGER_H

#include <Adafruit_NeoPixel.h>
#include "config.h"

class LedManager {
private:
  Adafruit_NeoPixel pixels;

public:
  LedManager();
  void begin();
  void setColor(uint8_t r, uint8_t g, uint8_t b);
  void clear();
};

extern LedManager statusLed;

#endif