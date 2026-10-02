/**
 * @file IndoorSensors.cpp
 * @brief Implementatie van kamersensoren (BMP085 barometer/temperatuur en DHT11 voor luchtvochtigheid) voor ESP32-S3.
 */

#include "Config.h"
#include "IndoorSensors.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include <DHT.h>
#include <Adafruit_BMP085.h>
#include <Wire.h>

// Globale variabelen worden beheerd in config.cpp / ClimateLogic.cpp
bool bmpConnected = false;
static unsigned long lastIndoorReadTime = 0;

// Hardware objecten
DHT indoorDht(INDOOR_DHT_PIN, INDOOR_DHT_TYPE);
Adafruit_BMP085 bmp;

/**
 * @brief Initialiseert de PIR bewegingssensor.
 */
void setupPIR() {
  pinMode(PIN_PIR, INPUT);
  logToSyslogAndSerial("[PIR] HW-416-B bewegingssensor geïnitialiseerd op Pin 1.");
}

/**
 * @brief Controleert of er beweging is gedetecteerd via de PIR sensor.
 */
void checkPIRMotion() {
  int motionState = digitalRead(PIN_PIR);
  if (motionState == HIGH) {
    lastMotionTime = millis();
    if (!isDisplayActiveByMotion) {
      isDisplayActiveByMotion = true;
      logToSyslogAndSerial("[PIR] Beweging gedetecteerd! Display ingeschakeld.");
    }
  } else {
    // Schakel display na 30 seconden inactiviteit uit
    if (isDisplayActiveByMotion && (millis() - lastMotionTime > 30000)) {
      isDisplayActiveByMotion = false;
      logToSyslogAndSerial("[PIR] Geen beweging meer. Display uitgeschakeld.");
    }
  }
}

/**
 * @brief Initialiseert de kamersensoren (DHT, BMP085 en PIR).
 */
void setupIndoorSensors() {
  // Initialiseer PIR
  setupPIR();

  // Start I2C met de juiste S3 pinnen gedefinieerd in config.h
  Wire.begin(PIN_SDA, PIN_SCL);
  
  indoorDht.begin();
  
  if (bmp.begin()) {
    bmpConnected = true;
    logToSyslogAndSerial("[INDOOR] BMP085 barometer succesvol gedetecteerd.");
  } else {
    logToSyslogAndSerial("[INDOOR WARNING] BMP085 barometer niet gevonden!");
  }
  
  logToSyslogAndSerial("[INDOOR] Kamersensoren geïnitialiseerd.");
}

/**
 * @brief Leest periodiek de kamersensoren uit (BMP voor temp/druk, DHT voor vochtigheid) en past smoothing toe.
 */
void updateIndoorSensors() {
  // Vergeet ook niet om eventueel checkPIRMotion() hier of in je main loop aan te roepen als dat nodig is!
  
  if (millis() - lastIndoorReadTime >= 3000 || lastIndoorReadTime == 0) {
    lastIndoorReadTime = millis();
    
    totalIndoorDhtReads++; 

    // 1. Sensoren uitlezen
    float rawHum = indoorDht.readHumidity();
    float rawTempBmp = bmpConnected ? bmp.readTemperature() : NAN;
    float rawPressure = bmpConnected ? (bmp.readPressure() / 100.0F) : NAN;

    // Validatie
    bool humValid = !isnan(rawHum) && (rawHum >= 0.0 && rawHum <= 100.0);
    bool tempValid = !isnan(rawTempBmp);

    // 2. Temperatuur vanuit BMP verwerken + offset
    if (tempValid) {
      float t = rawTempBmp + INDOOR_TEMP_OFFSET;
      indoorTemp = t;

      if (indoorSmoothedTemp == -999.0 || indoorSmoothedTemp == 0.0) {
        indoorSmoothedTemp = t;
      } else {
        indoorSmoothedTemp = (indoorSmoothedTemp * 0.8) + (t * 0.2);
      }
    }

    // 3. Luchtvochtigheid vanuit Indoor DHT verwerken + offset
    if (humValid) {
      float h = rawHum + INDOOR_HUM_OFFSET;
      indoorHum = h;

      if (indoorSmoothedHum == -999.0 || indoorSmoothedHum == 0.0) {
        indoorSmoothedHum = h;
      } else {
        indoorSmoothedHum = (indoorSmoothedHum * 0.8) + (h * 0.2);
      }
    } else {
      failedIndoorDhtReads++;
    }

    // 4. Dauwpunt, VPD en marges berekenen
    if (indoorSmoothedTemp != -999.0 && indoorSmoothedHum != -999.0) {
      float correctedIndoorTemp = indoorSmoothedTemp;
      float correctedIndoorHum = indoorSmoothedHum;

      indoorDewPoint = calcDewPoint(correctedIndoorTemp, correctedIndoorHum);
      indoorVpd = calcVPD(correctedIndoorTemp, correctedIndoorHum);

      indoorDpMargin = correctedIndoorTemp - indoorDewPoint;

      if (indoorDpMargin < 3.0) {
        indoorDpIcon = "🔴 (Kritiek)";
      } else if (indoorDpMargin < 5.0) {
        indoorDpIcon = "🟡 (Let op)";
      } else {
        indoorDpIcon = "🟢 (Veilig)";
      }

      if (indoorVpd < 0.6 || indoorVpd > 1.5) {
        indoorVpdIcon = "🔴";
      } else if (indoorVpd < 0.8) {
        indoorVpdIcon = "🟡";
      } else {
        indoorVpdIcon = "🟢";
      }
    }

    // 5. Barometer / Luchtdruk bijwerken indien verbonden
    if (!isnan(rawPressure) && rawPressure > 800.0) {
      #ifdef myAltitude
        currentPressure = rawPressure * pow(1.0 - (0.0065 * myAltitude) / (rawTempBmp + 273.15), -5.255);
      #else
        currentPressure = rawPressure;
      #endif

      currentPressure += PRESSURE_OFFSET; // Past automatisch de correctie toe
    }
  }
}