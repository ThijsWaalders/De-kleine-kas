/**
 * @file ClimateLogic.h
 * @brief Headerbestand voor klimaatberekeningen, VPD, dauwpunt, schimmelrisico en sturingslogica (ESP32-S3).
 */

#ifndef CLIMATELOGIC_H
#define CLIMATELOGIC_H

#include "Config.h"
#include "Logger.h"

// Externe objecten uit andere modules
extern PicoSyslog::Logger syslog;
extern SSD1306Wire display; 
extern DHT dht; 
extern Adafruit_BMP085 bmp;
extern BH1750 lightMeter;
extern UniversalTelegramBot bot;

// KLIMAAT & TREND VARIABELEN
extern unsigned long lastTrendSample;
extern const char* baroTrendArrow; 
extern float dailyHighTempKas;
extern float dailyLowTempKas;
extern float dailyHighTempRoom;
extern float dailyLowTempRoom;
extern unsigned long lastHighLowReset;

extern float stableLuxBase;
extern bool lastLightStateOn;
extern unsigned long luxShiftStartTime;
extern bool isLuxShiftPending;
extern bool isDisplayOff;
extern unsigned long darkStartTime;

// Globale sensorwaarden en statussen
extern float kasDewPoint;
extern float kasVpd;
extern float currentPressure;
extern float currentLuxValue; 
extern bool moldRisk;
extern String moldReasonText;
extern String vpdStatusText;
extern String dpMarginStatusText;

extern bool previousMoldRisk; 
extern bool moldHistory[MOLD_SAMPLES];
extern int moldSampleIndex;
extern bool moldHistoryFilled;
extern unsigned long lastMoldSampleTime;

// PROACTIEVE TREND & SNELHEIDSBEWAKING (VELOCITY)
extern float previousHumForVelocity;
extern unsigned long lastVelocityCheckTime;
extern float humidityVelocity; 

extern unsigned long lastBannerToggle;
extern int bannerMode; 

// ADVIES & BESTURING
extern HouseVentState houseAdvice;
extern KasVentState   kasAdvice;
extern HouseVentState previousHouseAdvice;
extern KasVentState   previousKasAdvice;

extern String houseAdviceReason;
extern String kasAdviceReason;

extern bool isSpaceHeatingNeeded;
extern bool isHeatMatRecommended;

extern bool isTestModeActive;
extern unsigned long testModeStartTime;
extern unsigned long currentTestDuration; 
extern unsigned long heatMatLowStartTime;    
extern unsigned long heatMatHighStartTime;  

extern float pastBaro;
extern float pastHum;
extern float pastTemp;

// Vochtigheid & Min/Max
extern float minIndoorHum, maxIndoorHum;
extern float minKasHum, maxKasHum;
extern float minOutdoorHum, maxOutdoorHum;
extern float roomHum;

void updateHumidityHighLow(float kasHum, float roomHum, float outHum);

// Forward declaration voor alert functionaliteit
void sendTelegramAlert(String message);

// inline float getCalibratedVcc() {
//   const float KALIBRATIE_FACTOR = 3.3F / 3.011F;
//   return (ESP.getVcc() / 1000.0F) * KALIBRATIE_FACTOR; 
// }

inline float calcDewPoint(float tempC, float hum) {
  if (isnan(tempC) || isnan(hum) || hum <= 0) return -999.0;
  float a = 17.27, b = 237.7;
  float alpha = ((a * tempC) / (b + tempC)) + log(hum / 100.0);
  return (b * alpha) / (a - alpha);
}

inline float calcVPD(float tempC, float hum) {
  if (isnan(tempC) || isnan(hum)) return -999.0;
  float svp = 0.61078 * exp((17.27 * tempC) / (tempC + 237.3));
  float avp = svp * (hum / 100.0);
  return svp - avp;
}

void evaluateClimateState(float tempC, float hum);
void checkClimateVelocity(float currentHum);
void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp);
void updateHeatingAdvice(float inTemp, float outTemp);
void updateMoldRiskHistory(bool currentRisk);
void checkKasTrends(float currentTemp, float currentHum, float currentBaro);
void checkRoomTrends();
void updateHighLow(float currentTemp);

#endif // CLIMATELOGIC_H