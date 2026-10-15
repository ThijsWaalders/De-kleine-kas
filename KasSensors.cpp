/**
 * @file KasSensors.cpp
 * @brief Implementatie van de kas-sensoren (SHT31 en BH1750) voor ESP32-S3.
 */

#include "Config.h"
#include "KasSensors.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <BH1750.h>

// Globale variabelen
bool lightMeterConnected = false;
// bool sht31Connected = false;

// Hardware objecten voor de kas
// Adafruit_SHT31 sht31 = Adafruit_SHT31();
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

      // DIRECT de pure waarde + je vaste offset. Geen fan-offsets of smoothing meer!
      float t = rawTemp + KAS_TEMP_OFFSET;
      float h = rawHum + KAS_HUM_OFFSET;

      kasTemp = t;
      kasHum = h;
      kasSmoothedTemp = t; // Direct doorzetten zonder vertraging
      kasSmoothedHum  = h;  // Direct doorzetten zonder vertraging

      kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
      kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
      displayedTemp = kasSmoothedTemp;

    } else {
      failedKasReads++;
      logToSyslogAndSerial("[KAS WARNING] SHT31 leest ongeldige data uit (NAN)!");
    }

    if (lightMeterConnected && !isnan(rawLux) && rawLux >= 0.0) {
      currentLuxValue = rawLux;
      
      // Lamp-status detectie & transitie trigger
      bool currentLightState = (currentLuxValue > 1.0);
      if (currentLightState != lampsAreOn) {
        lampsAreOn = currentLightState;
        lampTransitionStartTime = millis();
        logToSyslogAndSerial(lampsAreOn ? "[PREDICTIVE] Lampen AANGESLAGEN. Start 15m warmte-transitie." : "[PREDICTIVE] Lampen UITGESCHAKELD. Start afkoel-transitie.");
      }
    }

    // Controleer of de transitie-tijd inmiddels verstreken is
    if (isLampTransitionActive && (millis() - lampTransitionStartTime >= LAMP_TRANSITION_DURATION)) {
      isLampTransitionActive = false;
      logToSyslogAndSerial("[PREDICTIVE] Lamp-transitie periode afgerond. Klimaat gestabiliseerd.");
    } else if (!isLampTransitionActive && (millis() - lampTransitionStartTime < LAMP_TRANSITION_DURATION) && (millis() > lampTransitionStartTime)) {
      isLampTransitionActive = true;
    }
    // if (lightMeterConnected && !isnan(rawLux) && rawLux >= 0.0) {
    //   currentLuxValue = rawLux;
      
    // }
  }
}