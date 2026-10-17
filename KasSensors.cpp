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

// Variabelen voor de automatische SHT31 condens-heater check
static unsigned long lightsOutTimestamp = 0;
static bool heaterExecutedTonight = false;
static bool isHeaterCheckRunning = false;

/**
 * @brief Voert de SHT31 heater-check uit om condens weg te branden.
 * Schakelt tijdens de test tijdelijk de ventilatoren uit om koeling door luchtstroom te voorkomen.
 */
void checkAndRunShtHeater() {
  if (isHeaterCheckRunning) return;

  // Controleer of de lampen uit staan en de wachttijd van 30 minuten (1800000 ms) verstreken is
  if (!lampsAreOn && lightsOutTimestamp > 0 && !heaterExecutedTonight) {
    if (millis() - lightsOutTimestamp >= 1800000UL) {
      isHeaterCheckRunning = true;
      logToSyslogAndSerial("[HEATER] ⚡ Automatische SHT31 condens-heater gestart...");

      // 1. Bewaar de huidige ventilator-snelheden
      int prevFanInt  = fanIntSpeed;
      int prevFanExt1 = fanExt1Speed;
      int prevFanExt2 = fanExt2Speed;

      // 2. Schakel alle fans tijdelijk uit (geen luchtstroom die de chip koelt)
      fanIntSpeed  = 0;
      fanExt1Speed = 0;
      fanExt2Speed = 0;
      ledcWrite(PIN_FAN_INT_PWM,  0);
      ledcWrite(PIN_FAN_EXT1_PWM, 0);
      ledcWrite(PIN_FAN_EXT2_PWM, 0);
      delay(1000); // Wacht kort tot de luchtstroom stilstaat

      // 3. Schakel de SHT31 heater in voor 60 seconden
      if (sht31Connected) {
        sht31.heater(true);
        logToSyslogAndSerial("[HEATER] SHT31 verwarmingselement ACTIEF (1 minuut)...");
        delay(60000); // 1 minuut verwarmen
        sht31.heater(false);
        logToSyslogAndSerial("[HEATER] SHT31 verwarmingselement UITGESCHAKELD.");
      }

      // 4. Geef de sensor 2 minuten de tijd om af te koelen naar de echte omgevingstemperatuur
      delay(120000);

      // 5. Herstel de ventilator-snelheden
      fanIntSpeed  = prevFanInt;
      fanExt1Speed = prevFanExt1;
      fanExt2Speed = prevFanExt2;

      heaterExecutedTonight = true;
      lightsOutTimestamp = 0;
      isHeaterCheckRunning = false;
      logToSyslogAndSerial("[HEATER] ✅ Condens-check afgerond, normale klimaatregeling hervat.");
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

      // --- 24-UURS EXTREMEN BORGING (Geen -999.0 meer!) ---
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
        isLampTransitionActive = true; // Start de adaptieve transitiefase

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

    // Controleer of de automatische heater-check uitgevoerd moet worden
    checkAndRunShtHeater();
  }
}