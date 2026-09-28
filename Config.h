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
#include <Adafruit_BMP085.h>
#include <BH1750.h>
#include <PubSubClient.h>
#include <math.h>
#include <LittleFS.h>

// =========================================================================
// 1. HARDWARE & PIN DEFINITIES (ESP32-S3 Indeling)
// =========================================================================
const int FLASH_BUTTON_PIN  = 0;  // BOOT knop op ESP32-S3

// I2C bus pinnen (OLED display en sensoren zoals BMP085, BH1750)
#define PIN_SDA 4
#define PIN_SCL 5

// Ventilator PWM en Tacho pinnen
#define PIN_FAN_INT_TACHO   13  // Tacho GROEN voor interne ventilator
#define PIN_FAN_INT_PWM     12  // PWM-sturing BLAUW voor interne ventilator
#define PIN_FAN_EXT2_TACHO   11  // Tacho GROEN voor externe ventilator(en)
#define PIN_FAN_EXT2_PWM     10  // PWM-sturing BLAUW voor externe ventilator(en)
#define PIN_FAN_EXT1_TACHO   9  // Tacho GROEN voor externe ventilator(en)
#define PIN_FAN_EXT1_PWM     8  // PWM-sturing BLAUW voor externe ventilator(en)

// Sensor Pinnen & Typen (DHT11)
#define KAS_DHT_PIN         6   
#define KAS_DHT_TYPE        DHT11
#define INDOOR_DHT_PIN      7   
#define INDOOR_DHT_TYPE     DHT11

// Status LED (indien van toepassing)
#define PIN_NEOPIXEL        21  // Let op: als GPIO 21 al voor tacho wordt gebruikt, kun je deze eventueel naar een andere vrije pin verhuizen!
#define NUMPIXELS           1
#define LED_BRIGHTNESS      5

// =========================================================================
// 2. KLIMAAT & SENSOR CORRECTIE OFFSETS
// =========================================================================
const float KAS_TEMP_OFFSET    = +1.2; // Temperatuurcorrectie kas (°C)
const float INDOOR_TEMP_OFFSET = -0.8; // Temperatuurcorrectie woning (°C)
const float KAS_HUM_OFFSET     = -2.8; // Luchtvochtigheidcorrectie kas (%)
const float INDOOR_HUM_OFFSET  = +0.4; // Luchtvochtigheidcorrectie woning (%)

// =========================================================================
// 3. KLIMAAT & STURING DREMPELWAARDEN
// =========================================================================
const float GREENHOUSE_MAX_TEMP     = 22.5; 
const float VPD_MIN_OPTIMAL         = 0.40; 
const float VPD_MAX_OPTIMAL         = 1.0;  
const float DP_MARGIN_MIN           = 2.5;  
const float HUM_MOLD_THRESHOLD      = 68.0; 

// Warmtemat instellingen
const float HEAT_MAT_TEMP_LOW       = 16.0; 
const float HEAT_MAT_TEMP_HIGH      = 21.5; 
const unsigned long HEAT_MAT_DELAY  = 300000; 

// Buffer en schimmel historie constanten
const int MOLD_SAMPLES              = 60;
const int MAX_BUFFER_SIZE           = 50;
extern const char* BUFFER_FILE;

// Display en Hysteresis constanten
const unsigned long TEMP_UPDATE_INTERVAL = 5000;
const float TEMP_HYSTERESIS_THRESHOLD = 0.3;
const unsigned long BUTTON_DEBOUNCE_DELAY = 50;
const unsigned long LUX_STABILITY_TIMEOUT = 10000;

// --- PWM Configuraties voor Ventilatoren (ESP32 ledc) ---
const int PWM_FREQ                  = 25000; 
const int PWM_RANGE                 = 255; 
const int FAN_MAX_PWM               = 255; 

// Externe ventilatoren (Grote model)
const int EXT_FAN_BASE_PWM          = 70;  //65
const int EXT_FAN_MIN_PWM           = 70;  //65 ~25% minimale startdrempel extern

// Interne ventilator (Kleinere model - draait meer toeren)
const int INT_FAN_BASE_PWM          = 200;  // Eventueel eigen basis
const int INT_FAN_MIN_PWM           = 100;  // 55 = 750 RPM Eigen minimale startdrempel (pas aan naar wens voor de kleine fan)
// const int CIRCULATION_PWM           = 145;  // was 90 Vaste rust-stand voor de interne fan (pas dit getal aan zodat hij fijn zacht circuleert)

// =========================================================================
// 4. NETWERK, MQTT & TELEGRAM CONFIGURATIE
// =========================================================================
const char* const mqtt_server       = "192.168.1.110";
const int mqtt_port                 = 1883;
const char* const mqtt_topic        = "weerstation/data";
const char* const mqtt_status_topic = "weerstation/status"; 
const unsigned long MQTT_INTERVAL   = 30000; 

const unsigned long TELEGRAM_MIN_INTERVAL    = 4000; 
const unsigned long BOT_CHECK_INTERVAL       = 4000;  
const unsigned long MAX_TOTAL_WIFI_DOWN_TIME = 600000; 
const unsigned long MAX_WIFI_RECONNECT_TIME  = 30000;  

// =========================================================================
// 5. ENUMS & GLOBAL EXTERN DECLARATIES
// =========================================================================
enum HouseVentState { HOUSE_CLOSED, HOUSE_VENTILATE };
enum KasVentState   { OFF, GREENHOUSE_CIRCULATE_INTERNAL, KasVentState_dummy, GREENHOUSE_VENTILATE };

// --- Netwerk & API variabelen ---
extern char ssid[];
extern char password[];
extern String openWeatherKey, cityID;
extern char telbot[], telid[];     

// --- Systeem & Hardware Objecten ---
extern unsigned long lastWifiLedBlink;
extern bool wifiLedState;
extern SSD1306Wire display; 
extern DHT dht; 
extern Adafruit_BMP085 bmp;
extern BH1750 lightMeter;
extern WiFiClientSecure telegramSslClient;
extern UniversalTelegramBot bot;
extern WiFiClient mqttWifiClient;
extern PubSubClient mqttClient;

// Wi-Fi drop tracking variabelen
inline bool hadWifiDrop = false;
inline unsigned long wifiTotalDropStartTime = 0;

// --- Systeem & Netwerk Staten ---
extern unsigned long lastSuccessfulNetworkActivity;
extern bool isConnected;
// extern float espVcc;
extern int wifiRSSI;
extern uint32_t freeHeap;

// --- Meetwaarden & Klimaat (Extern) ---
extern float kasTemp, kasHum, indoorTemp, indoorHum;
extern float outdoorTemp, outdoorHumidity, outdoorDewPoint, outdoorPressure, outdoorVpd;
extern float kasDewPoint, kasVpd, indoorDewPoint, indoorVpd;

// Marges en Iconen
extern float kasDpMargin;
extern float indoorDpMargin;
extern String kasDpIcon;
extern String indoorDpIcon;
extern String indoorVpdIcon;

// Schimmel & Status teksten
extern bool moldRisk;
extern String moldReasonText, vpdStatusText, dpMarginStatusText;

// Min/Max Statistieken
extern float kasLowHum, kasHighHum, indoorLowHum, indoorHighHum, outdoorLowHum, outdoorHighHum;
extern float kasTempLow, kasTempHigh, indoorTempHigh, indoorTempLow;

// Smoothed waarden
extern float kasSmoothedTemp, kasSmoothedHum, indoorSmoothedTemp, indoorSmoothedHum, displayedTemp;
extern unsigned long lastTempUpdate;

// Sensor foutentellers
extern unsigned long totalKasDhtReads, failedKasDhtReads, lastValidKasDhtTime;
extern unsigned long totalIndoorDhtReads, failedIndoorDhtReads, lastValidIndoorDhtTime;

// Weer & Trends
extern float outdoorTempMax, outdoorTempMin;
extern bool outdoorMoldRisk, hasOutdoorAlert;
extern String weatherDesc, outdoorPressureTrendText;
extern int weatherID;
extern unsigned long lastWeatherUpdate;
extern float forecastedTempSoon;
extern bool isTempDroppingSoon;

// Besturing & Advies Staten
extern HouseVentState houseAdvice;
extern KasVentState kasAdvice;
extern bool pendingTelegramAlert;
extern bool bmpConnected;
extern bool lightMeterConnected;
extern bool isSpaceHeatingNeeded;
extern bool isHeatMatRecommended;
extern bool isTestModeActive;
extern unsigned long testModeStartTime, currentTestDuration;
extern unsigned long heatMatLowStartTime, heatMatHighStartTime;

// Trends & High/Low
extern float pastBaro, pastHum, pastTemp;
extern unsigned long lastTrendSample;
extern const char* baroTrendArrow;

// Display & Licht
extern float stableLuxBase;
extern bool lastLightStateOn;
extern unsigned long luxShiftStartTime;
extern bool isLuxShiftPending;
extern bool isDisplayOff;
extern unsigned long darkStartTime;

// Schimmel historie
extern bool previousMoldRisk;
extern bool moldHistory[MOLD_SAMPLES];
extern int moldSampleIndex;
extern bool moldHistoryFilled;
extern unsigned long lastMoldSampleTime;

// UI / Knoppen / Timers
extern unsigned long lastBannerToggle;
extern int bannerMode;
extern unsigned long lastButtonPressTime;
extern unsigned long lastBotCheckTime;
extern unsigned long lastTelegramSentTime;
extern unsigned long lastMqttUpdate;
extern float previousHumForVelocity;
extern unsigned long lastVelocityCheckTime;
extern float humidityVelocity;

// Ventilators & PID
// Ventilators & PID
extern volatile unsigned long rpmCountInt;
extern volatile unsigned long rpmCountExt1;
extern volatile unsigned long rpmCountExt2;
extern int fanIntSpeed, fanIntRPM, fanExt1Speed, fanExt1RPM, fanExt2Speed, fanExt2RPM;
extern float pidSetPointVPD, kp, ki, kd, pidOutput;
extern unsigned long lastPidTime;
extern float pError, iError, dError, lastError;

// --- TELEGRAM / ADVIES BUFFER ---
extern char advBuffer[1600];

#endif // CONFIG_H