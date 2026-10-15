/**
 * @file IndoorSensors.cpp
 * @brief Implementatie van kamersensoren (BME280 en PIR) voor ESP32-S3.
 */

#include "Config.h"
#include "IndoorSensors.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// bool bmeConnected = false;
static unsigned long lastIndoorReadTime = 0;

// Adafruit_BME280 bme; // BME280 voor binnen

void setupPIR() {
  pinMode(PIN_PIR, INPUT);
  logToSyslogAndSerial("[PIR] HW-416-B bewegingssensor geïnitialiseerd op Pin 1.");
}

void checkPIRMotion() {
  int motionState = digitalRead(PIN_PIR);
  if (motionState == HIGH) {
    lastMotionTime = millis();
    if (!isDisplayActiveByMotion) {
      isDisplayActiveByMotion = true;
      logToSyslogAndSerial("[PIR] Beweging gedetecteerd! Display ingeschakeld.");
    }
  } else {
    if (isDisplayActiveByMotion && (millis() - lastMotionTime > 30000)) {
      isDisplayActiveByMotion = false;
      logToSyslogAndSerial("[PIR] Geen beweging meer. Display uitgeschakeld.");
    }
  }
}

void setupIndoorSensors() {
  setupPIR();
  Wire.begin(PIN_SDA, PIN_SCL);
  
  if (bme.begin(0x76) || bme.begin(0x77)) {
    bmeConnected = true;
    logToSyslogAndSerial("[INDOOR] BME280 sensor succesvol gedetecteerd.");
  } else {
    logToSyslogAndSerial("[INDOOR WARNING] BME280 sensor niet gevonden!");
  }
  
  logToSyslogAndSerial("[INDOOR] Kamersensoren geïnitialiseerd.");
}

void updateIndoorSensors() {
  if (millis() - lastIndoorReadTime >= 3000 || lastIndoorReadTime == 0) {
    lastIndoorReadTime = millis();
    
    totalIndoorReads++;

    float rawTemp     = bmeConnected ? bme.readTemperature() : NAN;
    float rawHum      = bmeConnected ? bme.readHumidity() : NAN;
    float rawPressure = bmeConnected ? (bme.readPressure() / 100.0F) : NAN;

    bool tempValid = !isnan(rawTemp);
    bool humValid  = !isnan(rawHum) && (rawHum >= 0.0 && rawHum <= 100.0);
    bool pressValid = !isnan(rawPressure) && rawPressure > 800.0;

    if (!tempValid || !humValid) {
      failedIndoorReads++;
    } else {
      lastValidIndoorTime = millis();
    }

    if (tempValid) {
      // Direct rauw + offset = definitieve schone temperatuur
      float t = rawTemp + INDOOR_TEMP_OFFSET;
      indoorTemp = t;
      indoorSmoothedTemp = t; // Of lichte smoothing indien gewenst
    }

    if (humValid) {
      // Direct rauw + offset = definitieve schone luchtvochtigheid
      float h = rawHum + INDOOR_HUM_OFFSET;
      indoorHum = h;
      indoorSmoothedHum = h;
    }

    // GEEN dubbele offsets meer hier! Gewoon direct rekenen met de schone waarden:
    if (indoorSmoothedTemp != -999.0 && indoorSmoothedHum != -999.0) {
      indoorDewPoint = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
      indoorVpd = calcVPD(indoorSmoothedTemp, indoorSmoothedHum);
      indoorDpMargin = indoorSmoothedTemp - indoorDewPoint;

      indoorDpIcon = (indoorDpMargin < 3.0) ? "🔴 (Kritiek)" : (indoorDpMargin < 5.0) ? "🟡 (Let op)" : "🟢 (Veilig)";
      indoorVpdIcon = (indoorVpd < 0.1 || indoorVpd > 1.3) ? "🔴" : (indoorVpd < 0.4 || indoorVpd > 1.0) ? "🟡" : "🟢";
    }

    if (pressValid) {
      #ifdef myAltitude
        currentPressure = rawPressure * pow(1.0 - (0.0065 * myAltitude) / (rawTemp + 273.15), -5.255);
      #else
        currentPressure = rawPressure;
      #endif
      currentPressure += PRESSURE_OFFSET; 
    }
  }
}