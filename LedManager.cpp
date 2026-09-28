#include "LedManager.h"

// Maakt gebruik van de definities uit config.h
LedManager statusLed();

LedManager::LedManager() 
  : pixels(NUMPIXELS, PIN_NEOPIXEL, NEO_RGB + NEO_KHZ800) {}

void LedManager::begin() {
  pixels.begin();
  pixels.setBrightness(LED_BRIGHTNESS);
  pixels.clear();
  pixels.show();
}

void LedManager::setColor(uint8_t r, uint8_t g, uint8_t b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show();
}

void LedManager::clear() {
  pixels.clear();
  pixels.show();
}