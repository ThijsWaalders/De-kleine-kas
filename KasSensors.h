/**
 * @file KasSensors.h
 * @brief Headerbestand voor de kas sensoren (SHT31, BH1750) voor ESP32-S3.
 */

#ifndef KASSENSORS_H
#define KASSENSORS_H

#include <Arduino.h>
#include "Config.h" // Bevat alle centrale includes en pin-definities
#include <Adafruit_SHT31.h>
#include <BH1750.h>

// Externe hardware objecten
extern BH1750 lightMeter;
extern Adafruit_SHT31 sht31;

// Externe variabelen
extern float kasSmoothedTemp;
extern float kasSmoothedHum;
extern bool lightMeterConnected;
extern bool sht31Connected;
extern float currentLuxValue;

// Functie prototypes
void setupKasSensors();
void updateKasSensors();

#endif