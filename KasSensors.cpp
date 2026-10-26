/**
 * @file KasSensors.cpp
 * @brief Implementatie van de kas-sensoren (SHT31, BH1750 en 2x DS18B20) met lamp-transitie detectie, SHT31 heater-check en veilige tray-fallback voor ESP32-S3.
 */

#include "Config.h"
#include "KasSensors.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include "Telegram.h"
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Hardware objecten voor de kas
BH1750 lightMeter;

// 1-Wire setup voor DS18B20 sensoren (Gedeeld op PIN_DS18B20)
OneWire oneWire(PIN_DS18B20);
DallasTemperature dsSensors(&oneWire);

static unsigned long lastKasReadTime = 0;
static unsigned long lastTrayReadTime = 0;

// Variabelen voor de automatische & handmatige SHT31 condens-heater check
static unsigned long lightsOutTimestamp = 0;
static bool heaterExecutedTonight = false;
static bool isHeaterCheckRunning = false;

// State Machine voor Non-Blocking Heater Routine
enum HeaterState {
  HEATER_IDLE,
  HEATER_PREPARE_FANS,
  HEATER_HEATING,
  HEATER_COOLING
};

static HeaterState currentHeaterState = HEATER_IDLE;
static unsigned long heaterStateTimer = 0;
static int savedFanInt = 0, savedFanExt1 = 0, savedFanExt2 = 0;

/**
 * @brief Handmatige trigger voor SHT31 heater (bijv. via /cal)
 */
bool triggerManualShtHeater() {
  if (lampsAreOn) {
    logToSyslogAndSerial("[HEATER] Handmatige trigger afgewezen: Kweeklampen branden nog.");
    return false; 
  }

  if (isHeaterCheckRunning) {
    logToSyslogAndSerial("[HEATER] Heater-check draait al.");
    return true;
  }

  isHeaterCheckRunning = true;
  currentHeaterState = HEATER_PREPARE_FANS;
  heaterStateTimer = millis();
  logToSyslogAndSerial("[HEATER] ⚡ Handmatige SHT31 condens-heater gestart via /cal...");
  return true;
}

/**
 * @brief Non-blocking afhandeling van de SHT31 heater cyclus
 */
void handleShtHeaterStateMachine() {
  if (!isHeaterCheckRunning) return;

  switch (currentHeaterState) {
    case HEATER_PREPARE_FANS:
      savedFanInt  = fanIntSpeed;
      savedFanExt1 = fanExt1Speed;
      savedFanExt2 = fanExt2Speed;

      fanIntSpeed  = 0;
      fanExt1Speed = 0;
      fanExt2Speed = 0;
      ledcWrite(PIN_FAN_INT_PWM,  0);
      ledcWrite(PIN_FAN_EXT1_PWM, 0);
      ledcWrite(PIN_FAN_EXT2_PWM, 0);

      heaterStateTimer = millis();
      currentHeaterState = HEATER_HEATING;
      
      if (sht31Connected) {
        sht31.heater(true);
        logToSyslogAndSerial("[HEATER] SHT31 verwarmingselement ACTIEF (1 minuut)...");
      }
      break;

    case HEATER_HEATING:
      if (millis() - heaterStateTimer >= 60000UL) {
        if (sht31Connected) {
          sht31.heater(false);
          logToSyslogAndSerial("[HEATER] SHT31 verwarmingselement UITGESCHAKELD. Start 2 min afkoelfase...");
        }
        heaterStateTimer = millis();
        currentHeaterState = HEATER_COOLING;
      }
      break;

    case HEATER_COOLING:
      if (millis() - heaterStateTimer >= 120000UL) {
        fanIntSpeed  = savedFanInt;
        fanExt1Speed = savedFanExt1;
        fanExt2Speed = savedFanExt2;

        isHeaterCheckRunning = false;
        currentHeaterState = HEATER_IDLE;
        logToSyslogAndSerial("[HEATER] ✅ Condens-check afgerond, normale klimaatregeling hervat.");
      }
      break;

    default:
      break;
  }
}

/**
 * @brief Automatische periodieke check
 */
void checkAndRunShtHeater() {
  handleShtHeaterStateMachine();

  if (isHeaterCheckRunning) return;

  if (!lampsAreOn && lightsOutTimestamp > 0 && !heaterExecutedTonight) {
    if (millis() - lightsOutTimestamp >= 1800000UL) {
      heaterExecutedTonight = true;
      triggerManualShtHeater();
    }
  }
}

void setupKasSensors() {
  if (sht31.begin(0x44)) {
    sht31Connected = true;
    sht31.heater(false); 
    logToSyslogAndSerial("[KAS] SHT31 sensor gedetecteerd.");
  } else {
    logToSyslogAndSerial("[KAS WARNING] SHT31 niet gevonden op 0x44!");
  }

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    lightMeterConnected = true;
    logToSyslogAndSerial("[KAS] BH1750 lichtsensor gedetecteerd.");
  } else {
    lightMeterConnected = false;
    logToSyslogAndSerial("[KAS WARNING] BH1750 lichtsensor niet gevonden.");
  }

  // --- ROBUUSTE DS18B20 INITIALISATIE MET RETRY-LOOP ---
  dsSensors.begin();
  int deviceCount = 0;
  int retries = 5;

  while (retries > 0) {
    deviceCount = dsSensors.getDeviceCount();
    if (deviceCount >= 1) {
      logToSyslogAndSerialPrintf("[KAS] 🟢 1-Wire bus stabiel! %d DS18B20 sensor(en) gevonden.", deviceCount);
      break;
    }
    logToSyslogAndSerialPrintf("[KAS WARNING] ⚠️ 1-Wire init poging mislukt (%d sensoren). Nog %d pogingen...", deviceCount, retries - 1);
    delay(300);
    dsSensors.begin();
    retries--;
  }

  traySensor1Connected = (deviceCount >= 1);
  traySensor2Connected = (deviceCount >= 2);

  if (deviceCount == 0) {
    logToSyslogAndSerial("[KAS WARNING] ⚠️ Geen DS18B20 tray sensoren gevonden, fallback actief.");
  }
  
  logToSyslogAndSerial("[KAS] Kassensoren geïnitialiseerd.");
}

void updateKasSensors() {
  unsigned long currentMillis = millis();

  // --- 1. SHT31 & BH1750 UITLEZEN (Elke 3 seconden) ---
  if (currentMillis - lastKasReadTime >= 3000 || lastKasReadTime == 0) {
    lastKasReadTime = currentMillis;
    
    totalKasReads++;

    float rawTemp = sht31Connected ? sht31.readTemperature() : NAN;
    float rawHum  = sht31Connected ? sht31.readHumidity() : NAN;
    float rawLux  = lightMeterConnected ? lightMeter.readLightLevel() : 0.0;

    bool tempValid = !isnan(rawTemp);
    bool humValid  = !isnan(rawHum) && (rawHum >= 0.0 && rawHum <= 100.0);

    if (tempValid && humValid) {
      lastValidKasTime = currentMillis;

      float t = rawTemp + KAS_TEMP_OFFSET;
      float h = rawHum + KAS_HUM_OFFSET;

      kasTemp = t;
      kasHum = h;
      kasSmoothedTemp = t; 
      kasSmoothedHum  = h; 

      kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
      kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
      displayedTemp = kasSmoothedTemp;

      // --- 24-UURS EXTREMEN BORGING ---
      if (currentMillis > 3000) {
        static bool kasExtremesInit = false;
        if (!kasExtremesInit || kasTempLow == 999.0f || kasTempLow == -999.0f) {
          kasTempLow = t;
          kasTempHigh = t;
          kasLowHum  = h;
          kasHighHum  = h;
          kasExtremesInit = true;
        } else {
          if (t < kasTempLow)  kasTempLow = t;
          if (t > kasTempHigh) kasTempHigh = t;
          if (h < kasLowHum)   kasLowHum  = h;
          if (h > kasHighHum)  kasHighHum  = h;
        }
      }

    } else {
      failedKasReads++;
      logToSyslogAndSerial("[KAS WARNING] SHT31 leest ongeldige data uit (NAN)!");
    }

    // --- LICHTSENSOR & LAMP-TRANSITIE DETECTIE ---
    if (lightMeterConnected && !isnan(rawLux) && rawLux >= 0.0) {
      currentLuxValue = rawLux;

      bool currentLightState = (currentLuxValue > 1.0);
      if (currentLightState != lampsAreOn) {
        lampsAreOn = currentLightState;
        lampTransitionStartTime = currentMillis;
        isLampTransitionActive = true; 

        if (lampsAreOn) {
          logToSyslogAndSerial("[PREDICTIVE] 💡 Lampen AANGESLAGEN. Start adaptieve warmte-transitie.");
          sendTelegramAlert("💡 *Lampen AANGESLAGEN*\nDe kleine kas start de adaptieve transitie.");
        } else {
          lightsOutTimestamp = currentMillis;
          heaterExecutedTonight = false; 
          logToSyslogAndSerial("[PREDICTIVE] 🌙 Lampen UITGESCHAKELD. Timer (30 min) voor SHT31 condens-heater gestart.");
          sendTelegramAlert("🌙 *Lampen UITGESCHAKELD*\nDe kleine kas start de afkoel-transitie.");
        }
      }
    }
  }

  // --- 2. DS18B20 TRAY SENSOR(EN) UITLEZEN (Elke 5 seconden) ---
  if (currentMillis - lastTrayReadTime >= 5000 || lastTrayReadTime == 0) {
    lastTrayReadTime = currentMillis;

    dsSensors.requestTemperatures();
    
    // --- TRAY 1 ---
    float t1 = dsSensors.getTempCByIndex(0);
    if (t1 != DEVICE_DISCONNECTED_C && t1 > -55.0 && t1 < 125.0) {
      traySensor1Temp = t1 + TRAY_SENSOR_1_OFFSET; 
      traySensor1Connected = true;
      
      if (traySensor1Temp < traySensor1TempLow)  traySensor1TempLow = traySensor1Temp;
      if (traySensor1Temp > traySensor1TempHigh) traySensor1TempHigh = traySensor1Temp;
    } else {
      // Sensor is niet bereikbaar; markeer als offline i.p.v. een neppe 20 graden opleggen
      traySensor1Connected = false;
      traySensor1Temp = -999.0; // Geeft helder aan dat hij stuk/weg is
    }

    // --- TRAY 2 ---
    float t2 = dsSensors.getTempCByIndex(1);
    if (t2 != DEVICE_DISCONNECTED_C && t2 > -55.0 && t2 < 125.0) {
      traySensor2Temp = t2 + TRAY_SENSOR_2_OFFSET; 
      traySensor2Connected = true;
      
      if (traySensor2Temp < traySensor2TempLow)  traySensor2TempLow = traySensor2Temp;
      if (traySensor2Temp > traySensor2TempHigh) traySensor2TempHigh = traySensor2Temp;
    } else {
      traySensor2Connected = false;
      traySensor2Temp = -999.0;
    }    
  }

  // Controleer & verwerk de SHT31 heater-check (non-blocking)
  checkAndRunShtHeater();
}

float getTrayMinTemp(int mode) {
  if (mode == 0) return 17.5f; // Kiem
  if (mode == 1) return 15.0f; // Groei
  if (mode == 2) return 18.0f; // Warmtemat
  if (mode == 3) return -999.0f; // Uit
  return 15.0f;                
}

float getTrayMaxTemp(int mode) {
  if (mode == 0) return 23.5f; // Kiem
  if (mode == 1) return 25.0f; // Groei
  if (mode == 2) return 29.0f; // Warmtemat
  if (mode == 3) return -999.0f; // Uit
  return 25.0f;                
}