/**
 * @file IndoorSensors.h
 * @brief Headerbestand voor kamersensoren (ESP32-S3).
 */

#ifndef INDOORSENSORS_H
#define INDOORSENSORS_H

#include <Arduino.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>

// Externe variabelen
extern float indoorTemp;
extern float indoorHum;
extern float indoorSmoothedTemp;
extern float indoorSmoothedHum;
extern float indoorDewPoint;
extern float indoorVpd;
extern float currentPressure;
extern bool bmpConnected;

extern unsigned long totalIndoorDhtReads;
extern unsigned long failedIndoorDhtReads;

extern bool isDisplayActiveByMotion;

// Functies
void setupIndoorSensors();
void updateIndoorSensors();
void setupPIR();
void checkPIRMotion();

#endif 