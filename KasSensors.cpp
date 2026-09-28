/**
 * @file KasSensors.cpp
 * @brief Implementatie van de kas-sensoren (DHT en optioneel BH1750 lichtsensor) meting, smoothing, offsets, dauwpunt en VPD voor ESP32-S3.
 */

#include "Config.h"
#include "KasSensors.h"
#include "ClimateLogic.h"
#include "Logger.h"

// Daadwerkelijke definitie van de globale variabele voor de kas lichtsensor
bool lightMeterConnected = false;

// Hardware objecten voor de kas
DHT kasDht(KAS_DHT_PIN, KAS_DHT_TYPE);
BH1750 lightMeter;

static unsigned long lastKasReadTime = 0;

/**
 * @brief Initialiseert de kas sensoren.
 */
void setupKasSensors() {
  kasDht.begin();

  // Als je de lichtsensor weer wilt aanzetten, kun je hieronder de coderegel activeren:
  // if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
  //   lightMeterConnected = true;
  //   logToSyslogAndSerial("[KAS] BH1750 lichtsensor gedetecteerd.");
  // } else {
  //   lightMeterConnected = false;
  //   logToSyslogAndSerial("[KAS WARNING] BH1750 lichtsensor niet gevonden.");
  // }
  
  lightMeterConnected = false; // Forceer op false tot je hem fysiek aansluit
  logToSyslogAndSerial("[KAS] Kassensoren geïnitialiseerd.");
}

/**
 * @brief Leest periodiek de kas-sensoren uit, past offsets en smoothing toe.
 */
void updateKasSensors() {
  if (millis() - lastKasReadTime >= 3000 || lastKasReadTime == 0) {
    lastKasReadTime = millis();
    
    totalKasDhtReads++;

    float rawHum = kasDht.readHumidity();
    float rawTemp = kasDht.readTemperature();
    float rawLux = lightMeterConnected ? lightMeter.readLightLevel() : 0.0;

    bool humValid = !isnan(rawHum) && (rawHum >= 0.0 && rawHum <= 100.0);
    bool tempValid = !isnan(rawTemp);

    if (humValid && tempValid) {
      // 1. Offsets toepassen vanuit config.h
      float t = rawTemp + KAS_TEMP_OFFSET;
      float h = rawHum + KAS_HUM_OFFSET;

      kasTemp = t;
      kasHum = h;

      // 2. Smoothing toepassen
      if (kasSmoothedTemp == -999.0 || kasSmoothedTemp == 0.0) {
        kasSmoothedTemp = t;
        kasSmoothedHum = h;
      } else {
        kasSmoothedTemp = (kasSmoothedTemp * 0.8) + (t * 0.2);
        kasSmoothedHum  = (kasSmoothedHum * 0.8) + (h * 0.2);
      }

      // 3. Afgeleide waardes berekenen
      kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
      kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
      
      // Weergave temperatuur bijwerken
      displayedTemp = kasSmoothedTemp;

    } else {
      failedKasDhtReads++;
    }

    // Lichtsensor bijwerken indien verbonden
    if (lightMeterConnected && !isnan(rawLux) && rawLux >= 0.0) {
      currentLuxValue = rawLux;
    }
  }
}