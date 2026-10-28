#ifndef ESP_SENSORS_H
#define ESP_SENSORS_H

#include <Arduino.h>

void initEspSensors();
float readEspInternalTemp();
void checkEspTemperatureSafety();

extern float espInternalTemp;
extern bool isFakeEspOverheatActive;

#endif // ESP_SENSORS_H

// #ifndef ESP_SENSORS_H
// #define ESP_SENSORS_H

// #include <Arduino.h>

// void initEspSensors();
// float readEspInternalTemp();
// void updateEspSensors()
// void checkEspTemperatureSafety();

// extern float espInternalTemp;
// extern bool isFakeEspOverheatActive;

// #endif