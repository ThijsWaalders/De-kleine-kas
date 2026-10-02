#ifndef ESP_SENSORS_H
#define ESP_SENSORS_H

#include <Arduino.h>

void initEspSensors();
float readEspInternalTemp();
void checkEspTemperatureSafety();

extern float espInternalTemp;

#endif