/**
 * @file Config.h
 * @brief Centrale configuratie, pin-definities en globale instellingen voor de kascontroller (ESP32-S3).
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include "nono.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Wire.h>        
#include <SSD1306Wire.h>
#include "DHT.h"
#include "driver/temperature_sensor.h"
#include <Adafruit_BMP085.h>
#include <BH1750.h>
#include <PubSubClient.h>
#include <math.h>
#include <LittleFS.h>

#define ENABLE_TELEGRAM true

// =========================================================================
// 1. HARDWARE & PIN DEFINITIES (ESP32-S3 Indeling)
// =========================================================================
const int FLASH_BUTTON_PIN  = 0;  // Boot knop op ESP32-S3
#define PIN_PIR             2     // HW-416-B Bewegingssensor
#define PIN_SCL             4
#define PIN_SDA             5

// Pinnen voor Ventilatoren (PWM & Tacho)
#define PIN_FAN_INT_TACHO   13   
#define PIN_FAN_INT_PWM     12   
#define PIN_FAN_EXT1_TACHO  11  
#define PIN_FAN_EXT1_PWM    10  
#define PIN_FAN_EXT2_TACHO  9  
#define PIN_FAN_EXT2_PWM    8  

// Sensor Pinnen & Typen
#define INDOOR_DHT_PIN      6   
#define INDOOR_DHT_TYPE     DHT11
#define KAS_DHT_PIN         7   
#define KAS_DHT_TYPE        DHT11
#define myAltitude          21.5 // Hoogte in meters boven zeeniveau

// Status LED (NeoPixel)
#define PIN_NEOPIXEL        21  
#define NUMPIXELS           1
#define LED_BRIGHTNESS      5


// =========================================================================
// 2. STATUS LED CONFIGURATIE
// =========================================================================
enum LedMode { OFF, SOLID, BLINK, BREATHE };

namespace LedConfig {
  struct LedSetting {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t brightness;
    LedMode mode;
    unsigned long interval;
  };

  constexpr LedSetting WIFI_DISCONNECTED = {   0,   0, 180,   5, BLINK,  300 };
  constexpr LedSetting WEATHER_ALARM   = { 255, 180,   0,   4, BREATHE, 3000 };
  constexpr LedSetting HOUSE_ADVICE    = { 255,  90,   0,   5, SOLID,     0 };
  constexpr LedSetting SYSTEM_ACTIVE   = {   0,   0,   0,   0, OFF,     0 }; 
  constexpr LedSetting ALL_OK_IDLE     = {   0,   0,   0,   0, OFF,     0 };
}


// =========================================================================
// 3. KIEMGROENTEN KLIMAAT & STURING CONFIGURATIE
// =========================================================================
const float VPD_MAX_OPTIMAL                 = 0.85;  
const float VPD_MIN_OPTIMAL                 = 0.55;  
const float VPD_ORANGE_OFFSET               = 0.00;  
const float GREENHOUSE_MAX_TEMP             = 26.0;  
const float GREENHOUSE_CRASH_TEMP           = 28.0;  

const float OUTDOOR_MAX_HUMIDITY_FOR_VENT   = 85.0; 
const float HUM_MOLD_THRESHOLD              = 75.0;  
const float DP_MARGIN_MIN                   = 2.0;        

const float HEAT_MAT_TEMP_LOW               = 16.0;   
const float HEAT_MAT_TEMP_HIGH              = 20.0;  
const unsigned long HEAT_MAT_DELAY          = 300000; 

const int MOLD_SAMPLES                      = 60;
const int MAX_BUFFER_SIZE                   = 50;

const unsigned long TEMP_UPDATE_INTERVAL    = 5000;
const float TEMP_HYSTERESIS_THRESHOLD       = 0.3;
const unsigned long BUTTON_DEBOUNCE_DELAY   = 50;
const unsigned long LUX_STABILITY_TIMEOUT   = 10000;

// Sensor Correctie Offsets
const float KAS_TEMP_OFFSET                 = +0.3; 
const float KAS_HUM_OFFSET                  = -5.0; 
const float INDOOR_TEMP_OFFSET              = -1.8; 
const float INDOOR_HUM_OFFSET               = +1.4; 
const float PRESSURE_OFFSET                 = -1.8; 


// =========================================================================
// 4. VENTILATOR & PWM HARDWARE CONFIGURATIE
// =========================================================================
const int PWM_FREQ                          = 25000; 
const int PWM_RANGE                         = 255; 
const int FAN_MAX_PWM                       = 255; 

const int EXT_FAN_BASE_PWM                  = 123;  
const int EXT_FAN_MIN_PWM                   = 120;  
const int INT_FAN_BASE_PWM                  = 111; 
const int INT_FAN_MIN_PWM                   = 65;  

const unsigned long MIN_FAN_RUN_TIME        = 180000; 
const unsigned long SYSTEM_STARTUP_DELAY    = 120000;


// =========================================================================
// 5. NETWERK, MQTT & TELEGRAM CONFIGURATIE
// =========================================================================
const char* const mqtt_server               = "192.168.1.110";
const int mqtt_port                         = 1883;
const char* const mqtt_topic                = "weerstation/data";
const char* const mqtt_status_topic         = "weerstation/status"; 
const unsigned long MQTT_INTERVAL           = 30000; 

const unsigned long TELEGRAM_MIN_INTERVAL    = 1000; 
const unsigned long BOT_CHECK_INTERVAL       = 1000; 
const unsigned long MAX_TOTAL_WIFI_DOWN_TIME = 600000; 
const unsigned long MAX_WIFI_RECONNECT_TIME  = 30000;  


// =========================================================================
// 6. ENUMS & STATEN
// =========================================================================
enum HouseVentState { HOUSE_CLOSED, HOUSE_VENTILATE };
enum KasVentState   { KAS_OFF, GREENHOUSE_CIRCULATE_INTERNAL, KasVentState_dummy, GREENHOUSE_VENTILATE };


// =========================================================================
// 7. EXTERN DECLARATIES (Globale variabelen beheerd in Config.cpp)
// =========================================================================
extern const float ESP_CRITICAL_TEMP;
extern unsigned long lastEspTempCheck;
inline bool fanAlertsEnabled = true;

extern bool mfOverrideActive;
extern bool isKasSleeping;
extern bool pendingTelegramAlert;
extern bool homeVentActive; // Extern gemaakt om multiple definitions te voorkomen

extern char ssid[];
extern char password[];
extern String openWeatherKey, cityID;
extern char telbot[], telid[];     

extern unsigned long lastWifiLedBlink;
extern bool wifiLedState;

extern SSD1306Wire display; 
extern DHT dht; 
extern Adafruit_BMP085 bmp;
extern BH1750 lightMeter;
extern WiFiClientSecure telegramSslClient;
extern UniversalTelegramBot bot;
extern PubSubClient mqttClient;

inline bool hadWifiDrop = false;
inline unsigned long wifiTotalDropStartTime = 0;

extern unsigned long lastSuccessfulNetworkActivity;
extern bool isConnected;
extern int wifiRSSI;
extern uint32_t freeHeap;
extern const char* BUFFER_FILE;

extern float kasTemp, kasHum, indoorTemp, indoorHum;
extern float outdoorTemp, outdoorHumidity, outdoorDewPoint, outdoorPressure, outdoorVpd;
extern float kasDewPoint, kasVpd, indoorDewPoint, indoorVpd;

extern float kasDpMargin;
extern float indoorDpMargin;
extern float outdoorDpMargin;
extern String kasDpIcon;
extern String indoorDpIcon;
extern String indoorVpdIcon;

extern bool moldRisk;
extern String moldReasonText, vpdStatusText, dpMarginStatusText;

extern float kasLowHum, kasHighHum, indoorLowHum, indoorHighHum, outdoorLowHum, outdoorHighHum;
extern float kasTempLow, kasTempHigh, indoorTempHigh, indoorTempLow;

extern float kasSmoothedTemp, kasSmoothedHum, indoorSmoothedTemp, indoorSmoothedHum, displayedTemp;
extern unsigned long lastTempUpdate;

extern unsigned long totalKasDhtReads, failedKasDhtReads, lastValidKasDhtTime;
extern unsigned long totalIndoorDhtReads, failedIndoorDhtReads, lastValidIndoorDhtTime;

extern float outdoorTempMax, outdoorTempMin;
extern bool outdoorMoldRisk, hasOutdoorAlert;
extern String weatherDesc, outdoorPressureTrendText;
extern int weatherID;
extern unsigned long lastWeatherUpdate;
extern float forecastedTempSoon;
extern bool isTempDroppingSoon;

extern HouseVentState houseAdvice;
extern KasVentState kasAdvice;
extern bool bmpConnected;
extern bool lightMeterConnected;
extern bool isSpaceHeatingNeeded;
extern bool isHeatMatRecommended;
extern bool isTestModeActive;
extern unsigned long testModeStartTime, currentTestDuration;
extern unsigned long heatMatLowStartTime, heatMatHighStartTime;

extern float pastBaro, pastHum, pastTemp;
extern unsigned long lastTrendSample;
extern const char* baroTrendArrow;

extern float stableLuxBase;
extern bool lastLightStateOn;
extern unsigned long luxShiftStartTime;
extern bool isLuxShiftPending;
extern bool isDisplayOff;
extern bool isDisplayActiveByMotion;
extern unsigned long lastMotionTime;
extern unsigned long darkStartTime;

extern bool previousMoldRisk;
extern bool moldHistory[MOLD_SAMPLES];
extern int moldSampleIndex;
extern bool moldHistoryFilled;
extern unsigned long lastMoldSampleTime;

extern unsigned long lastBannerToggle;
extern int bannerMode;
extern unsigned long lastButtonPressTime;
extern unsigned long lastBotCheckTime;
extern unsigned long lastTelegramSentTime;
extern unsigned long lastMqttUpdate;
extern float previousHumForVelocity;
extern unsigned long lastVelocityCheckTime;
extern float humidityVelocity;

extern volatile unsigned long rpmCountInt;
extern volatile unsigned long rpmCountExt1;
extern volatile unsigned long rpmCountExt2;
extern int fanIntSpeed, fanIntRPM, fanExt1Speed, fanExt1RPM, fanExt2Speed, fanExt2RPM;
extern int fanIntPct;
extern int fanExt1Pct;
extern int fanExt2Pct;

extern float pidSetPointVPD, kp, ki, kd, pidOutput;
extern unsigned long lastPidTime;
extern float pError, iError, dError, lastError;

extern char advBuffer[1600]; // Extern gemaakt om multiple definitions te voorkomen

// Functies
float readEspInternalTemp();

#endif // CONFIG_H