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
#include <Adafruit_SHT31.h>
#include <Adafruit_BME280.h>
#include "driver/temperature_sensor.h"
#include <BH1750.h>
#include <PubSubClient.h>
#include <math.h>
#include <Preferences.h>
#include <LittleFS.h>

#define FIRMWARE_VERSION "v3.5.5"
#define BUILD_TIMESTAMP  __DATE__ " " __TIME__

#define ENABLE_TELEGRAM true
#define VPD_CRITICAL_HIGH 1.00f  // Vanaf hier springt het scherm op hard alarm

// --- FEEDFORWARD LAMP ANTICIPATIE ---
#define PREEMPTIVE_LAMP_LOOKAHEAD_MINUTES 180 // 3 uur van tevoren anticiperen op de lampen
extern bool isPreemptiveLampConditioningActive; // Vlag voor actieve anticipatiefase
extern int testMinutesUntilLightsOn;             // Testvariabele om het te testen

// =========================================================================
// 🎛️ DEEL 1: GEBRUIKERSINSTELLINGEN & GRENSWAARDEN (CENTRALE BRON)
// =========================================================================
const unsigned long CLIMATE_CHECK_INTERVAL_MS    = 15000; // 15 seconden
const unsigned long MQTT_SEND_INTERVAL_MS        = 30000; // 30 seconden

// --- Klimaatlogica & Veiligheid ---
const float GREENHOUSE_CRASH_OFFSET            = 2.0f;
const float SYSTEM_READY_LUX_THRESHOLD         = 1.0f;
const unsigned long SYSTEM_READY_SHORT_DELAY_MS = 5000UL;
const float HEAT_MAT_DISABLE_TEMP              = 22.0f;
const float HUMIDITY_VELOCITY_THRESHOLD        = 3.0f;
const float HUMIDITY_VELOCITY_MIN_HUM          = 60.0f;
const float BARO_TREND_THRESHOLD               = 0.2f;

// --- PID Regelaar Tuning (Ventilator-sturing op VPD) ---
inline float kp = 0.5f;       // Proportioneel
inline float ki = 0.15f;      // Integraal
inline float kd = 0.6f;       // Derivatief

// --- VPD & Klimaat Doelen (Lamp-Aware & Centraal) ---
const float VPD_MAX_ERROR_THRESHOLD            = 0.3f;
const float VPD_MIN_OPTIMAL_DAY                = 0.60f; // Met lampen AAN
const float VPD_MIN_OPTIMAL_NIGHT              = 0.50f; // Met lampen UIT (ruimer voor nachtdip)
const float VPD_MAX_OPTIMAL_DAY                = 1.25f; // Ruimer onder lampen
const float VPD_MAX_OPTIMAL_NIGHT              = 1.10f; // Standaard nacht max
const float pidSetPointVPD                     = 0.70f;

// --- Ventilator PWM Snelheden & Grenzen ---
const int FAN_MAX_PWM                          = 255;   
const int EXT_FAN_MIN_PWM                      = 65; 
const int EXT_FAN_BASE_PWM                     = 95; 
const int INT_FAN_BASE_PWM                     = 111;   
const int INT_FAN_MIN_PWM                      = 110;     

// --- Drempelwaarden & Veiligheid ---
const float GREENHOUSE_MAX_TEMP                = 25.0f;  
const float GREENHOUSE_CRASH_TEMP              = 28.0f;   
const float OUTDOOR_MAX_HUMIDITY_FOR_VENT      = 85.0f;  
const float HUM_MOLD_THRESHOLD                 = 80.0f;  // Verruimd naar 80% om valse klam-meldingen te voorkomen
const float DP_MARGIN_MIN                      = 2.0f;   
const float HEAT_MAT_TEMP_LOW                  = 0.0f;  
const float HEAT_MAT_TEMP_HIGH                 = 22.0f;  
const float HOUSE_VPD_MIN_TARGET               = 0.80f;  // Minimale gewenste woning-VPD
const float HOUSE_HUM_MAX_TARGET               = 62.0f;  // Woningvochtigheid grens voor ontluchting

// --- Sensor Correctie Offsets ---
const float KAS_TEMP_OFFSET                    = -0.5f;
const float INDOOR_TEMP_OFFSET                 = -2.0f;
const float TRAY_SENSOR_1_OFFSET               = +0.2f; 
const float TRAY_SENSOR_2_OFFSET               = +0.2f;
const float KAS_HUM_OFFSET                     = -1.8f;
const float INDOOR_HUM_OFFSET                  = +6.8f;
const float PRESSURE_OFFSET                    = -2.2f;
// #define FIRMWARE_VERSION "v3.5.5"
// #define BUILD_TIMESTAMP  __DATE__ " " __TIME__

// #define ENABLE_TELEGRAM true
// #define VPD_CRITICAL_HIGH 1.00f  // Vanaf hier springt het scherm op hard alarm

// // --- FEEDFORWARD LAMP ANTICIPATIE ---
// #define PREEMPTIVE_LAMP_LOOKAHEAD_MINUTES 180 // 3 uur van tevoren anticiperen op de lampen
// extern bool isPreemptiveLampConditioningActive; // Vlag voor actieve anticipatiefase
// extern int testMinutesUntilLightsOn;             // Testvariabele (bijv. 60 of 180 min) om het te testen

// // =========================================================================
// // 🎛️ DEEL 1: GEBRUIKERSINSTELLINGEN & GRENSWAARDEN
// // =========================================================================
// const unsigned long CLIMATE_CHECK_INTERVAL_MS    = 15000; // 15 seconden
// const unsigned long MQTT_SEND_INTERVAL_MS        = 30000; // 30 seconden

// // --- Klimaatlogica & Veiligheid ---
// const float GREENHOUSE_CRASH_OFFSET            = 2.0f;
// const float SYSTEM_READY_LUX_THRESHOLD         = 1.0f;
// const unsigned long SYSTEM_READY_SHORT_DELAY_MS = 5000UL;
// const float HEAT_MAT_DISABLE_TEMP              = 22.0f;
// const float HUMIDITY_VELOCITY_THRESHOLD        = 3.0f;
// const float HUMIDITY_VELOCITY_MIN_HUM          = 60.0f;
// const float BARO_TREND_THRESHOLD               = 0.2f;

// // --- PID Regelaar Tuning (Ventilator-sturing op VPD) ---
// inline float kp = 0.5f;       // Proportioneel
// inline float ki = 0.15f;      // Integraal
// inline float kd = 0.6f;       // Derivatief

// // --- VPD & Klimaat Doelen (Kiemgroenten) ---
// const float VPD_MAX_ERROR_THRESHOLD            = 0.3f;
// const float VPD_MAX_OPTIMAL                    = 0.95f;
// const float VPD_MIN_OPTIMAL                    = 0.50f;
// const float pidSetPointVPD                     = 0.70f;

// // --- Ventilator PWM Snelheden & Grenzen ---
// const int FAN_MAX_PWM                          = 255;   
// const int EXT_FAN_MIN_PWM                      = 65; 
// const int EXT_FAN_BASE_PWM                     = 95; 
// const int INT_FAN_BASE_PWM                     = 111;   
// const int INT_FAN_MIN_PWM                      = 110;     

// // --- Drempelwaarden & Veiligheid ---
// const float GREENHOUSE_MAX_TEMP                = 25.0;  
// const float GREENHOUSE_CRASH_TEMP              = 28.0;   
// const float OUTDOOR_MAX_HUMIDITY_FOR_VENT      = 85.0;  
// const float HUM_MOLD_THRESHOLD                 = 70.0;  
// const float DP_MARGIN_MIN                      = 2.0;   
// const float HEAT_MAT_TEMP_LOW                  = 0.0;  
// const float HEAT_MAT_TEMP_HIGH                 = 22.0;  

// // --- Sensor Correctie Offsets ---
// const float KAS_TEMP_OFFSET                    = -0.5;
// const float INDOOR_TEMP_OFFSET                 = -2.0;
// const float TRAY_SENSOR_1_OFFSET               = +0.2f; 
// const float TRAY_SENSOR_2_OFFSET               = +0.2f;
// const float KAS_HUM_OFFSET                     = -1.8;
// const float INDOOR_HUM_OFFSET                  = +6.8;
// const float PRESSURE_OFFSET                    = -2.2;


// =========================================================================
// ⚙️ DEEL 2: VASTE HARDWARE & PIN DEFINITIES (ESP32-S3 Indeling)
// =========================================================================
const int FLASH_BUTTON_PIN                     = 0;     
#define PIN_PIR                                2        
#define PIN_SCL                                4        
#define PIN_SDA                                5        
#define PIN_DS18B20                            7        

#define PIN_FAN_INT_TACHO                      13   
#define PIN_FAN_INT_PWM                        12   
#define PIN_FAN_EXT1_TACHO                     11  
#define PIN_FAN_EXT1_PWM                       10  
#define PIN_FAN_EXT2_TACHO                     9  
#define PIN_FAN_EXT2_PWM                       8  

#define myAltitude                             21.5 
#define PIN_NEOPIXEL                           21  
#define NUMPIXELS                              1
#define LED_BRIGHTNESS                         5

const int PWM_FREQ                             = 25000; 
const int PWM_RANGE                            = 255; 
const unsigned long MIN_FAN_RUN_TIME           = 45000; 
const unsigned long SYSTEM_STARTUP_DELAY       = 30000; 
const unsigned long TEMP_UPDATE_INTERVAL       = 5000;
const float TEMP_HYSTERESIS_THRESHOLD          = 0.3;
const unsigned long BUTTON_DEBOUNCE_DELAY      = 50;
const unsigned long LUX_STABILITY_TIMEOUT      = 10000;
const int MOLD_SAMPLES                         = 10; 
const int MAX_BUFFER_SIZE                      = 50;
const unsigned long HEAT_MAT_DELAY             = 300000; 


// =========================================================================
// 🌐 DEEL 3: NETWERK, MQTT & TELEGRAM CONFIGURATIE
// =========================================================================
const char* const mqtt_server                  = "192.168.1.110";
const int mqtt_port                            = 1883;
const char* const mqtt_topic                   = "weerstation/data";
const char* const mqtt_status_topic            = "weerstation/status"; 
const unsigned long MQTT_INTERVAL              = 30000; 

const unsigned long TELEGRAM_MIN_INTERVAL       = 1000; 
const unsigned long BOT_CHECK_INTERVAL          = 1000; 
const unsigned long MAX_TOTAL_WIFI_DOWN_TIME    = 600000; 
const unsigned long MAX_WIFI_RECONNECT_TIME     = 30000;  


// =========================================================================
// 🔤 DEEL 4: STATUS LEDS, ENUMS & STATEN
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
  constexpr LedSetting SYSTEM_ACTIVE   = {   0,   0,   0,   6, SOLID,     0 }; 
  constexpr LedSetting ALL_OK_IDLE     = {   0,   0,   0,   6, SOLID,     0 };
}

enum HouseVentState { HOUSE_CLOSED, HOUSE_VENTILATE };
enum KasVentState   { KAS_OFF, GREENHOUSE_CIRCULATE_INTERNAL, KasVentState_dummy, GREENHOUSE_VENTILATE };


// =========================================================================
// 📦 DEEL 5: EXTERN DECLARATIES (MACHINEKAMER KOPPELING)
// =========================================================================
const float ESP_CRITICAL_TEMP = 70.0;
extern unsigned long lastEspTempCheck;
inline bool fanAlertsEnabled = true;

extern Preferences preferences;
extern bool lightMeterConnected;
extern bool sht31Connected;
extern bool bmeConnected;
extern bool lampsAreOn;
extern bool isLampTransitionActive;
extern unsigned long lampTransitionStartTime;

extern bool mfOverrideActive;
extern bool isKasSleeping;
extern bool pendingTelegramAlert;
extern bool homeVentActive;

extern char ssid[];
extern char password[];
extern String openWeatherKey, cityID;
extern char telbot[], telid[];     

extern unsigned long lastWifiLedBlink;
extern bool wifiLedState;

extern SSD1306Wire display; 
extern Adafruit_SHT31 sht31; 
extern Adafruit_BME280 bme;
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
extern float outdoorTemp, outdoorHumidity, outdoorDewPoint, outdoorPressure, outdoorVpd, outdoorLowHum, outdoorHighHum;
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
extern bool traySensor1Connected, traySensor2Connected;
extern float traySensor1Temp, traySensor2Temp;
extern float traySensor1TempLow, traySensor1TempHigh;
extern float traySensor2TempLow, traySensor2TempHigh;

extern int tray1Mode;
extern int tray2Mode;

void saveTrayModesToPreferences();
void loadTrayModesFromPreferences();

extern unsigned long totalKasReads, failedKasReads, lastValidKasTime;
extern unsigned long totalIndoorReads, failedIndoorReads, lastValidIndoorTime;

extern float outdoorTempMax, outdoorTempMin;
extern bool outdoorMoldRisk, hasOutdoorAlert;
extern String weatherDesc, outdoorPressureTrendText;
extern int weatherID;
extern unsigned long lastWeatherUpdate;
extern float forecastedTempSoon;
extern bool isTempDroppingSoon;

extern bool isCalibrationModeActive;

extern HouseVentState houseAdvice;
extern String houseAdviceReason;
extern KasVentState kasAdvice;
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

extern unsigned long lastPidTime;
extern float pError, iError, dError, lastError;
extern float pidOutput;

extern char advBuffer[1600];

// Functie declaraties
float readEspInternalTemp();

#endif // CONFIG_H


// /**
//  * @file Config.h
//  * @brief Centrale configuratie, pin-definities en globale instellingen voor de kascontroller (ESP32-S3).
//  */

// #ifndef CONFIG_H
// #define CONFIG_H

// #include <Arduino.h>
// #include "nono.h"
// #include <WiFi.h>
// #include <WiFiUdp.h>
// #include <HTTPClient.h>
// #include <ArduinoJson.h> 
// #include <WiFiClientSecure.h>
// #include <UniversalTelegramBot.h>
// #include <Wire.h>        
// #include <SSD1306Wire.h>
// #include <Adafruit_SHT31.h>
// #include <Adafruit_BME280.h>
// #include "driver/temperature_sensor.h"
// #include <BH1750.h>
// #include <PubSubClient.h>
// #include <math.h>
// #include <Preferences.h>      // Slaapstand (isKasSleeping) Opslaan in NVS (Flash-geheugen)
// #include <LittleFS.h>

// #define FIRMWARE_VERSION "v3.5.2"
// #define BUILD_TIMESTAMP  __DATE__ " " __TIME__

// // SHT31 sensor check
// extern Preferences preferences;

// #define ENABLE_TELEGRAM true

// // =========================================================================
// // 🎛️ DEEL 1: GEBRUIKERSINSTELLINGEN (HIER AFSTELLEN WAT JE WILT WIJZIGEN)
// // =========================================================================
// // Kritieke VPD drempels voor alarmering en weergave
// #define VPD_CRITICAL_HIGH 1.00f  // Vanaf hier springt het scherm op hard alarm (Uitdroging)

// const unsigned long CLIMATE_CHECK_INTERVAL_MS = 15000; // Tijd tussen checks: 15 seconden (was 60s)
// const unsigned long MQTT_SEND_INTERVAL_MS = 30000; // Stuur elke 30 seconden data naar MQTT

// // --- Klimaatlogica & Veiligheid Grenswaarden ---
// const float GREENHOUSE_CRASH_OFFSET       = 2.0f;   // Extra marge boven crash temp
// const float SYSTEM_READY_LUX_THRESHOLD    = 1.0f;   // Lux drempel voor snelle opstart bij daglicht
// const unsigned long SYSTEM_READY_SHORT_DELAY_MS = 5000UL; // Snelle start delay bij licht (ms)
// const float HEAT_MAT_DISABLE_TEMP         = 22.0f;  // Harde bovengrens waarbij de warmtemat altijd uitschakelt
// const float HUMIDITY_VELOCITY_THRESHOLD   = 3.0f;   // Maximale stijging in %/min voor interne circulatie
// const float HUMIDITY_VELOCITY_MIN_HUM     = 60.0f;  // Minimale luchtvochtigheid om snelheidstrigger toe te staan
// const float BARO_TREND_THRESHOLD          = 0.2f;   // Luchtdrukverschil drempel voor trend (pijl omhoog/omlaag)


// // --- 1.1 VPD & Klimaat Doelen (Kiemgroenten) ---
// const float VPD_MAX_ERROR_THRESHOLD = 0.3f; // Foutmarge waarbij de ventilator direct naar max springt
// const float VPD_MAX_OPTIMAL                 = 0.95f; //0.95; //0.85;  // Iets strakker dan 0.94 om uitdroging door lampen voor te zijn
// const float VPD_MIN_OPTIMAL                 = 0.50f; //0.50; // Veilig boven de schimmelgrens (voorkom onder 0.45 = schimmelrisico!)
// const float pidSetPointVPD                  = 0.70f; //0.70; //0.68;  // Het ideale middelpunt voor kiemgroenten

// // // --- 1.2 PID Regelaar Tuning (Ventilator-sturing op VPD) ---
// inline float kp = 0.5; // was 1.5;       // Proportioneel
// inline float ki = 0.15;                    // Integraal
// inline float kd = 0.6;                     // Derivatief
// // =========================================================================
// // 1. PID REGELAAR VARIABELEN
// // =========================================================================
// // extern float kp;                                     
// // extern float ki;                                     
// // extern float kd;                                     

// // --- 1.3 Ventilator PWM Snelheden & Grenzen ---
// // FAN_MIN = De absolute harde ondergrens (hardware-bescherming om te voorkomen dat ze stilvallen).
// // FAN_BASE = Het startpunt van de schaling zodra er geventileerd moet worden, waarna ze opschalen naar FAN_MAX_PWM
// const int FAN_MAX_PWM                       = 255;   

// const int EXT_FAN_MIN_PWM                   = 65; //120;  om extreme tacho waardes te voorkomen ivm ruis e.d. 
// const int EXT_FAN_BASE_PWM                  = 95; //123;  om extreme tacho waardes te voorkomen ivm ruis e.d.

// const int INT_FAN_BASE_PWM                  = 111; // 111 = ~1600 rpm   
// const int INT_FAN_MIN_PWM                   = 110; //65;     

// // --- 1.4 Klimaat Drempelwaarden & Veiligheid ---
// const float GREENHOUSE_MAX_TEMP             = 25.0;  
// const float GREENHOUSE_CRASH_TEMP           = 28.0;   
// const float OUTDOOR_MAX_HUMIDITY_FOR_VENT   = 85.0;  
// const float HUM_MOLD_THRESHOLD              = 70.0; //75.0;  
// const float DP_MARGIN_MIN                   = 2.0;   

// const float HEAT_MAT_TEMP_LOW               = 0.0; //16.0;  
// const float HEAT_MAT_TEMP_HIGH              = 22.0; //20.0;  

// // --- 1.5 Sensor Correctie Offsets ---
// // --- TEMP
// const float KAS_TEMP_OFFSET                 = -0.5; // -0.6; // -0.9; // Kalibratie offset kas SHT31 temperatuur
// const float INDOOR_TEMP_OFFSET              = -2.0; // -2.1; // -2.3; // -2.2  // Kalibratie offset woning BME280 temperatuur
// // DS18B20 tray sensoren
// const float TRAY_SENSOR_1_OFFSET            = +0.2f; //-0.0f; 
// const float TRAY_SENSOR_2_OFFSET            = +0.2f; //-0.1f;
// // --- LUCHTVOCHTIGHEID
// const float KAS_HUM_OFFSET                  = -1.8; //-0.2; // -0.4; // Kalibratie offset kas SHT31 luchtvochtigheid
// const float INDOOR_HUM_OFFSET               = +6.8; // +7.4; // +8.3; // +6.9; // Kalibratie offset woning BME280 luchtvochtigheid
// // --- LUCHTDRUK
// const float PRESSURE_OFFSET                 = -2.2; // -2.2  // Kalibratie offset luchtdruk (BME280)


// // =========================================================================
// // ⚙️ DEEL 2: VASTE HARDWARE & PIN DEFINITIES (ESP32-S3 Indeling)
// // =========================================================================
// const int FLASH_BUTTON_PIN                  = 0;     // Boot knop op ESP32-S3
// #define PIN_PIR                             2        // HW-416-B Bewegingssensor
// #define PIN_SCL                             4        // I2C Clock (Gedeeld voor SHT31, BME280, BH1750)
// #define PIN_SDA                             5        // I2C Data  (Gedeeld voor SHT31, BME280, BH1750)
// #define PIN_DS18B20                         7        // 1-Wire B18D20's

// // Pinnen voor Ventilatoren (PWM & Tacho pulsen)
// #define PIN_FAN_INT_TACHO                   13   
// #define PIN_FAN_INT_PWM                     12   
// #define PIN_FAN_EXT1_TACHO                  11  
// #define PIN_FAN_EXT1_PWM                    10  
// #define PIN_FAN_EXT2_TACHO                  9  
// #define PIN_FAN_EXT2_PWM                    8  

// // Hoogte boven zeeniveau voor luchtdrukcorrectie (BME280)
// #define myAltitude                          21.5 

// // Status LED (NeoPixel)
// #define PIN_NEOPIXEL                        21  
// #define NUMPIXELS                           1
// #define LED_BRIGHTNESS                      5

// // PWM & Timing Hardware Configuratie
// const int PWM_FREQ                          = 25000; 
// const int PWM_RANGE                         = 255; 
// const unsigned long MIN_FAN_RUN_TIME        = 45000; //180000; // korter gezet voor snellere reactie v/d fans
// const unsigned long SYSTEM_STARTUP_DELAY    = 30000; //30 sec //120000; 2 min
// const unsigned long TEMP_UPDATE_INTERVAL    = 5000;
// const float TEMP_HYSTERESIS_THRESHOLD       = 0.3;
// const unsigned long BUTTON_DEBOUNCE_DELAY   = 50;
// const unsigned long LUX_STABILITY_TIMEOUT   = 10000;
// const int MOLD_SAMPLES                      = 10; //60;
// const int MAX_BUFFER_SIZE                   = 50;
// const unsigned long HEAT_MAT_DELAY          = 300000; 


// // =========================================================================
// // 🌐 DEEL 3: NETWERK, MQTT & TELEGRAM CONFIGURATIE
// // =========================================================================
// const char* const mqtt_server               = "192.168.1.110";
// const int mqtt_port                         = 1883;
// const char* const mqtt_topic                = "weerstation/data";
// const char* const mqtt_status_topic         = "weerstation/status"; 
// const unsigned long MQTT_INTERVAL           = 30000; 

// const unsigned long TELEGRAM_MIN_INTERVAL    = 1000; 
// const unsigned long BOT_CHECK_INTERVAL       = 1000; 
// const unsigned long MAX_TOTAL_WIFI_DOWN_TIME = 600000; 
// const unsigned long MAX_WIFI_RECONNECT_TIME  = 30000;  


// // =========================================================================
// // 🔤 DEEL 4: STATUS LEDS, ENUMS & STATEN
// // =========================================================================
// enum LedMode { OFF, SOLID, BLINK, BREATHE };

// namespace LedConfig {
//   struct LedSetting {
//     uint8_t red;
//     uint8_t green;
//     uint8_t blue;
//     uint8_t brightness;
//     LedMode mode;
//     unsigned long interval;
//   };

//   constexpr LedSetting WIFI_DISCONNECTED = {   0,   0, 180,   5, BLINK,  300 };
//   constexpr LedSetting WEATHER_ALARM   = { 255, 180,   0,   4, BREATHE, 3000 };
//   constexpr LedSetting HOUSE_ADVICE    = { 255,  90,   0,   5, SOLID,     0 };
//   constexpr LedSetting SYSTEM_ACTIVE   = {   0,   0,   0,   6, SOLID,     0 }; 
//   constexpr LedSetting ALL_OK_IDLE     = {   0,   0,   0,   6, SOLID,     0 };
//   // constexpr LedSetting SYSTEM_ACTIVE   = {   0,   0,   0,   0, OFF,     0 }; 
//   // constexpr LedSetting ALL_OK_IDLE     = {   0,   0,   0,   0, OFF,     0 };
// }

// enum HouseVentState { HOUSE_CLOSED, HOUSE_VENTILATE };
// enum KasVentState   { KAS_OFF, GREENHOUSE_CIRCULATE_INTERNAL, KasVentState_dummy, GREENHOUSE_VENTILATE };


// // =========================================================================
// // 📦 DEEL 5: EXTERN DECLARATIES (MACHINEKAMER KOPPELING)
// // =========================================================================
// const float ESP_CRITICAL_TEMP = 70.0;
// extern unsigned long lastEspTempCheck;
// inline bool fanAlertsEnabled = true;

// // LAMP-TRANSITIE & PREDICTIVE CONTROL CONFIGURATIE
// extern bool lightMeterConnected;
// extern bool sht31Connected;
// extern bool bmeConnected;
// extern bool lampsAreOn;
// extern bool isLampTransitionActive;
// extern unsigned long lampTransitionStartTime;
// // constexpr unsigned long LAMP_TRANSITION_DURATION = 900000UL; // 15 minuten (900.000 ms)

// extern bool mfOverrideActive;
// extern bool isKasSleeping;
// extern bool pendingTelegramAlert;
// extern bool homeVentActive;

// extern char ssid[];
// extern char password[];
// extern String openWeatherKey, cityID;
// extern char telbot[], telid[];     

// extern unsigned long lastWifiLedBlink;
// extern bool wifiLedState;

// extern SSD1306Wire display; 
// extern Adafruit_SHT31 sht31; 
// extern Adafruit_BME280 bme;
// extern BH1750 lightMeter;
// extern WiFiClientSecure telegramSslClient;
// extern UniversalTelegramBot bot;
// extern PubSubClient mqttClient;

// inline bool hadWifiDrop = false;
// inline unsigned long wifiTotalDropStartTime = 0;

// extern unsigned long lastSuccessfulNetworkActivity;
// extern bool isConnected;
// extern int wifiRSSI;
// extern uint32_t freeHeap;
// extern const char* BUFFER_FILE;

// extern float kasTemp, kasHum, indoorTemp, indoorHum;
// extern float outdoorTemp, outdoorHumidity, outdoorDewPoint, outdoorPressure, outdoorVpd, outdoorLowHum, outdoorHighHum;
// extern float kasDewPoint, kasVpd, indoorDewPoint, indoorVpd;

// extern float kasDpMargin;
// extern float indoorDpMargin;
// extern float outdoorDpMargin;
// extern String kasDpIcon;
// extern String indoorDpIcon;
// extern String indoorVpdIcon;

// extern bool moldRisk;
// extern String moldReasonText, vpdStatusText, dpMarginStatusText;

// extern float kasLowHum, kasHighHum, indoorLowHum, indoorHighHum, outdoorLowHum, outdoorHighHum;
// extern float kasTempLow, kasTempHigh, indoorTempHigh, indoorTempLow;

// extern float kasSmoothedTemp, kasSmoothedHum, indoorSmoothedTemp, indoorSmoothedHum, displayedTemp;
// extern unsigned long lastTempUpdate;
// extern bool traySensor1Connected, traySensor2Connected;
// extern bool isTray1GerminatingMode; // true = Kiemfase (17.5-23.5°C), false = Groeifase (15-25°C)
// extern bool isTray2GerminatingMode;
// extern float traySensor1Temp, traySensor2Temp;
// extern float traySensor1TempLow;
// extern float traySensor1TempHigh;
// extern float traySensor2TempLow;
// extern float traySensor2TempHigh;

// // Tray modi (0 = Kiemfase, 1 = Groeifase, 2 = Warmtemat-modus)
// extern int tray1Mode;
// extern int tray2Mode;

// void saveTrayModesToPreferences();
// void loadTrayModesFromPreferences();

// // Sensor uitlees- en foutentellers (SHT31 & BME280)
// extern unsigned long totalKasReads, failedKasReads, lastValidKasTime;
// extern unsigned long totalIndoorReads, failedIndoorReads, lastValidIndoorTime;

// extern float outdoorTempMax, outdoorTempMin;
// extern bool outdoorMoldRisk, hasOutdoorAlert;
// extern String weatherDesc, outdoorPressureTrendText;
// extern int weatherID;
// extern unsigned long lastWeatherUpdate;
// extern float forecastedTempSoon;
// extern bool isTempDroppingSoon;

// // Voor sensor kalibratie modus SHT31 heater test
// extern bool isCalibrationModeActive;

// extern HouseVentState houseAdvice;
// extern KasVentState kasAdvice;
// // extern bool bmeConnected;
// // extern bool sht31Connected;
// // extern bool lightMeterConnected;
// extern bool isSpaceHeatingNeeded;
// extern bool isHeatMatRecommended;
// extern bool isTestModeActive;
// extern unsigned long testModeStartTime, currentTestDuration;
// extern unsigned long heatMatLowStartTime, heatMatHighStartTime;

// extern float pastBaro, pastHum, pastTemp;
// extern unsigned long lastTrendSample;
// extern const char* baroTrendArrow;

// extern float stableLuxBase;
// extern bool lastLightStateOn;
// extern unsigned long luxShiftStartTime;
// extern bool isLuxShiftPending;
// extern bool isDisplayOff;
// extern bool isDisplayActiveByMotion;
// extern unsigned long lastMotionTime;
// extern unsigned long darkStartTime;

// extern bool previousMoldRisk;
// extern bool moldHistory[MOLD_SAMPLES];
// extern int moldSampleIndex;
// extern bool moldHistoryFilled;
// extern unsigned long lastMoldSampleTime;

// extern unsigned long lastBannerToggle;
// extern int bannerMode;
// extern unsigned long lastButtonPressTime;
// extern unsigned long lastBotCheckTime;
// extern unsigned long lastTelegramSentTime;
// extern unsigned long lastMqttUpdate;
// extern float previousHumForVelocity;
// extern unsigned long lastVelocityCheckTime;
// extern float humidityVelocity;

// extern volatile unsigned long rpmCountInt;
// extern volatile unsigned long rpmCountExt1;
// extern volatile unsigned long rpmCountExt2;
// extern int fanIntSpeed, fanIntRPM, fanExt1Speed, fanExt1RPM, fanExt2Speed, fanExt2RPM;
// extern int fanIntPct;
// extern int fanExt1Pct;
// extern int fanExt2Pct;

// extern unsigned long lastPidTime;
// extern float pError, iError, dError, lastError;
// extern float pidOutput;

// extern char advBuffer[1600];

// // Functie declaraties
// float readEspInternalTemp();

// #endif // CONFIG_H