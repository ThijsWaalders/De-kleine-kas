/**
 * @file KasSensors.cpp
 * @brief Implementatie van de kas-sensoren (SHT31 en BH1750) met lamp-transitie detectie voor ESP32-S3.
 */

#include "Config.h"
#include "KasSensors.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>

// Hardware objecten voor de kas
BH1750 lightMeter;

static unsigned long lastKasReadTime = 0;

void setupKasSensors() {
  if (sht31.begin(0x44)) {
    sht31Connected = true;
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
          logToSyslogAndSerial("[PREDICTIVE] 🌙 Lampen UITGESCHAKELD. Start adaptieve afkoel-transitie.");
          sendTelegramAlert("🌙 *Lampen UITGESCHAKELD*\nDe kleine kas start de afkoel-transitie.");
        }
      }
    }
  }
}