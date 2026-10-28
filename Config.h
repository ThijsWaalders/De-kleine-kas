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

#define FIRMWARE_VERSION "v3.6"
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

// =========================================================================
// 🏡 INDOOR & BUFFER CONFIGURATIE (Uniforme Standaard v3.5.6)
// =========================================================================
const float INDOOR_VPD_MIN_TARGET              = 0.72f;  // Realistische minimale indoor-VPD
const float INDOOR_HUM_MAX_TARGET              = 70.0f;  // Realistische indoor luchtvochtigheid grens
const float INDOOR_PROJECTED_HUM_MAX           = 70.0f;  // Geprojecteerde vochtigheidsgrens
const float INDOOR_DEWPOINT_MARGIN             = 1.5f;   // Minimale dauwpunt-marge t.o.v. buitenlucht
const unsigned long INDOOR_ESCALATION_TIMEOUT  = 900000UL; // 15 minuten evaluatievenster tegen spam

// --- Sensor Correctie Offsets ---
const float KAS_TEMP_OFFSET                    = -0.5f;
const float INDOOR_TEMP_OFFSET                 = -2.0f;
const float TRAY_SENSOR_1_OFFSET               = +0.2f; 
const float TRAY_SENSOR_2_OFFSET               = +0.2f;
const float KAS_HUM_OFFSET                     = -1.8f;
const float INDOOR_HUM_OFFSET                  = +6.8f;
const float PRESSURE_OFFSET                    = -2.2f;

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
