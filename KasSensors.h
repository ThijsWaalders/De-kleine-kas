/**
 * @file KasSensor.h
 * @brief Headerbestand voor de kas sensoren (DHT11, BH1750) voor ESP32-S3.
 */

#ifndef KASSENSORS_H
#define KASSENSORS_H

#include <Arduino.h>
#include "Config.h" // Bevat alle centrale includes en pin-definities

// Externe hardware objecten
extern BH1750 lightMeter;

// Externe variabelen
extern float kasSmoothedTemp;
extern float kasSmoothedHum;
extern bool lightMeterConnected;
extern float currentLuxValue;

// Functie prototypes
void setupKasSensors();
void updateKasSensors();

#endif 