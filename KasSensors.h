/**
 * @file KasSensors.h
 * @brief Headerbestand voor de kas sensoren (SHT31, BH1750, DS18B20x2) voor ESP32-S3.
 */

#ifndef KASSENSORS_H
#define KASSENSORS_H

#include <Arduino.h>
#include "Config.h" // Bevat alle centrale includes en pin-definities
#include <Adafruit_SHT31.h>
#include <BH1750.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Externe hardware objecten
extern BH1750 lightMeter;
extern Adafruit_SHT31 sht31;

// Externe variabelen
extern float kasSmoothedTemp;
extern float kasSmoothedHum;
extern float traySensor1Temp; // Tray sensor 1 (DS18B20)
extern float traySensor2Temp; // Tray sensor 2 (DS18B20)
extern bool lightMeterConnected;
extern bool sht31Connected;
extern bool traySensor1Connected;
extern bool traySensor2Connected;
extern float currentLuxValue;

// Functie prototypes
void setupKasSensors();
void updateKasSensors();
float getTrayMinTemp(int mode);
float getTrayMaxTemp(int mode);

#endif