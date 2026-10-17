/**
 * @file KasSensors.cpp
 * @brief Implementatie van de kas-sensoren (SHT31 en BH1750) met lamp-transitie detectie en SHT31 heater-check voor ESP32-S3.
 */

#include "Config.h"
#include "KasSensors.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include "Telegram.h"
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>

// Hardware objecten voor de kas
BH1750 lightMeter;

static unsigned long lastKasReadTime = 0;

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
    return false; // Mislukt omdat lampen aan staan
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
      // 1. Bewaar fan-snelheden en zet alles uit
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
      // 2. Laat de heater 60 seconden branden zonder de CPU te blokkeren
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
      // 3. Laat de sensor 2 minuten (120 seconden) afkoelen
      if (millis() - heaterStateTimer >= 120000UL) {
        // Herstel de originele ventilator snelheden
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
  // Verwerk eerst eventuele lopende stappen van de heater
  handleShtHeaterStateMachine();

  if (isHeaterCheckRunning) return;

  // Automatische trigger (30 min na lampen uit)
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
    sht31.heater(false); // Zorg dat heater standaard uit staat
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
  
  logToSyslogAndSerial("[KAS] Kassensoren geïnitialiseerd.");
}

void updateKasSensors() {
  if (millis() - lastKasReadTime >= 3000 || lastKasReadTime == 0) {
    lastKasReadTime = millis();
    
    totalKasReads++;

    float rawTemp = sht31Connected ? sht31.readTemperature() : NAN;
    float rawHum  = sht31Connected ? sht31.readHumidity() : NAN;
    float rawLux  = lightMeterConnected ? lightMeter.readLightLevel() : 0.0;

    bool tempValid = !isnan(rawTemp);
    bool humValid  = !isnan(rawHum) && (rawHum >= 0.0 && rawHum <= 100.0);

    if (tempValid && humValid) {
      lastValidKasTime = millis();

      // Rauwe meting + vaste kalibratie-offset
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
      if (millis() > 3000) {
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
        lampTransitionStartTime = millis();
        isLampTransitionActive = true; 

        if (lampsAreOn) {
          logToSyslogAndSerial("[PREDICTIVE] 💡 Lampen AANGESLAGEN. Start adaptieve warmte-transitie.");
          sendTelegramAlert("💡 *Lampen AANGESLAGEN*\nDe kleine kas start de adaptieve transitie.");
        } else {
          lightsOutTimestamp = millis();
          heaterExecutedTonight = false; // Reset voor de nieuwe nacht
          logToSyslogAndSerial("[PREDICTIVE] 🌙 Lampen UITGESCHAKELD. Timer (30 min) voor SHT31 condens-heater gestart.");
          sendTelegramAlert("🌙 *Lampen UITGESCHAKELD*\nDe kleine kas start de afkoel-transitie.");
        }
      }
    }

    // Controleer & verwerk de SHT31 heater-check (non-blocking)
    checkAndRunShtHeater();
  }
}