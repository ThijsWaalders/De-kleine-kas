/**
 * @file ClimateLogic.cpp
 * @brief Implementatie van robuuste klimaatberekeningen voor kiemgroenten (ESP32-S3) met gelaagde fan-sturing.
 *        Maakt strikt gebruik van de Single Source of Truth (Config.h).
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"
#include "LedManager.h"

extern LedManager statusLed;

// =========================================================================
// 1. VASTE CORRECTIES & KALIBRATIE
// =========================================================================
float kasTempOffset = 0.0; 
float kasHumOffset = 0.0;  

// Globale vlag voor de noodtoestand (automatisch herstel ingebouwd)
bool isClimateCrash = false; 

// =========================================================================
// 2. BUFFER / VOORTSCHRIJDEND GEMIDDELDE (Snellere respons)
// =========================================================================
#define SENSOR_BUFFER_SIZE 3 

struct SensorSample {
  float temp;
  float hum;
  float vpd;
  float dp;
};

static SensorSample kasBuffer[SENSOR_BUFFER_SIZE];
static int kasBufferIndex = 0;
static bool kasBufferFilled = false;
static unsigned long lastBufferSampleTime = 0;

// =========================================================================
// 3. ANTI-HUNT VARIABELEN VOOR VENTILATOR
// =========================================================================
unsigned long fanStateChangeTime = 0;
KasVentState lastAppliedKasAdvice = KAS_OFF;

// =========================================================================
// 4. HULPFUNCTIES: BUFFERING & GEMIDDELDES
// =========================================================================
void updateSensorBuffer(float rawTemp, float rawHum, float &avgTemp, float &avgHum, float &avgVpd, float &avgDp) {
  unsigned long currentMillis = millis();

  if (currentMillis - lastBufferSampleTime >= 20000 || lastBufferSampleTime == 0) {
    lastBufferSampleTime = currentMillis;

    float correctedTemp = rawTemp + kasTempOffset;
    float correctedHum = rawHum + kasHumOffset;

    kasBuffer[kasBufferIndex].temp = correctedTemp;
    kasBuffer[kasBufferIndex].hum = correctedHum;
    kasBuffer[kasBufferIndex].vpd = calcVPD(correctedTemp, correctedHum);
    kasBuffer[kasBufferIndex].dp = calcDewPoint(correctedTemp, correctedHum);

    kasBufferIndex = (kasBufferIndex + 1) % SENSOR_BUFFER_SIZE;
    if (kasBufferIndex == 0) kasBufferFilled = true;
  }

  int totalSamples = kasBufferFilled ? SENSOR_BUFFER_SIZE : kasBufferIndex;
  if (totalSamples == 0) {
    avgTemp = rawTemp + kasTempOffset;
    avgHum = rawHum + kasHumOffset;
    avgVpd = calcVPD(avgTemp, avgHum);
    avgDp = calcDewPoint(avgTemp, avgHum);
    return;
  }

  float sumTemp = 0, sumHum = 0, sumVpd = 0, sumDp = 0;
  for (int i = 0; i < totalSamples; i++) {
    sumTemp += kasBuffer[i].temp;
    sumHum += kasBuffer[i].hum;
    sumVpd += kasBuffer[i].vpd;
    sumDp += kasBuffer[i].dp;
  }

  avgTemp = sumTemp / totalSamples;
  avgHum = sumHum / totalSamples;
  avgVpd = sumVpd / totalSamples;
  avgDp = sumDp / totalSamples;
}

KasVentState applyAntiHuntFilter(KasVentState rawNewAdvice) {
  unsigned long currentMillis = millis();

  if (rawNewAdvice != lastAppliedKasAdvice) {
    if (currentMillis - fanStateChangeTime < MIN_FAN_RUN_TIME) {
      return lastAppliedKasAdvice; 
    }
    lastAppliedKasAdvice = rawNewAdvice;
    fanStateChangeTime = currentMillis;
    logToSerial("[ANTI-HUNT] Nieuwe ventilatorstatus geaccepteerd.");
  }

  return lastAppliedKasAdvice;
}

// =========================================================================
// 5. KLIMAAT CRASH BEWAKING (Noodtoestand met automatische herstel-check)
// =========================================================================
void checkClimateCrash(float inTemp) {
  // Maakt nu gebruik van Config.h definities i.p.v. hardcoded getallen
  bool criticalTempHigh = (inTemp >= (GREENHOUSE_CRASH_TEMP + 2.0)); // bijv. 30°C als crashgrens
  bool safeTempRecovery = (inTemp <= GREENHOUSE_CRASH_TEMP);       // bijv. herstel onder 28°C

  if (!isClimateCrash && criticalTempHigh) {
    isClimateCrash = true;
    logToSyslogAndSerialPrintf("[CRASH] KLIMAAT CRASH GEDETECTEERD! Kas temperatuur is kritiek: %.1f°C", inTemp);
    sendTelegramAlert("🚨 *KRITIEK KLIMAATALARM:* De kas is in een noodtoestand beland (Te heet: " + String(inTemp, 1) + "°C)! Systeem forceert maximale ventilatie.");
  } 
  else if (isClimateCrash && safeTempRecovery) {
    isClimateCrash = false;
    logToSyslogAndSerialPrintf("[RECOVERY] Klimaat crash voorbij. Kas temperatuur hersteld: %.1f°C", inTemp);
    sendTelegramAlert("✅ *HERSTEL:* De kas is weer buiten de gevarenzone (Temp: " + String(inTemp, 1) + "°C). Systeem hervat automatische regeling.");
  }
}

// =========================================================================
// 6. HOOFD KLIMAAT- & SCHIMMELDIAGNOSE
// =========================================================================
void evaluateClimateState(float tempC, float hum) {
  if (isnan(tempC) || isnan(hum)) return;

  if (millis() < 180000) {
    moldRisk = false;
    moldReasonText = "⏳ Systeem kalibreert / leert...";
    vpdStatusText = " `(⏳ Opstarten)`";
    dpMarginStatusText = " `(⏳ Kalibreren)`";
    return;
  }

  float avgTemp, avgHum, avgVpd, avgDp;
  updateSensorBuffer(tempC, hum, avgTemp, avgHum, avgVpd, avgDp);

  kasDewPoint = avgDp;
  kasVpd = avgVpd;
  kasDpMargin = avgTemp - kasDewPoint;

  checkClimateCrash(avgTemp);

  bool vpdTooWet = (kasVpd < VPD_MIN_OPTIMAL);
  bool vpdTooDry = (kasVpd > VPD_MAX_OPTIMAL);
  bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
  bool humHigh = (avgHum >= HUM_MOLD_THRESHOLD);
  bool tempTooLow = (avgTemp < HEAT_MAT_TEMP_LOW);
  bool tempTooHigh = (avgTemp > GREENHOUSE_MAX_TEMP);
  
  bool lampsAreOn = (currentLuxValue > 1.0);

  if (isClimateCrash) {
    moldRisk = true;
    moldReasonText = "🚨 NOODTOESTAND: Kas uit de hand gelopen (" + String(avgTemp, 1) + "°C)!";
  }
  else if (vpdTooWet) {
    moldRisk = true;
    moldReasonText = "⚠️ SCHIMMELRISICO: Kas te klam (VPD " + String(kasVpd, 2) + " < " + String(VPD_MIN_OPTIMAL, 2) + " kPa)";
  }
  else if (vpdTooDry) {
    moldRisk = false; 
    String cause = lampsAreOn ? "door brandende kweeklampen" : "door omgevingswarmte/droogte";
    moldReasonText = "⚠️ UITDROGINGSRISICO: Kas te droog " + cause + " (VPD " + String(kasVpd, 2) + " kPa)";
  }
  else if (condensRisk) {
    moldRisk = true;
    moldReasonText = "⚠️ KRITIEK: Condensgevaar op zaadjes! Marge: " + String(kasDpMargin, 1) + "°C";
  } 
  else if (humHigh) {
    moldRisk = true;
    moldReasonText = "⚠️ RISICO: Luchtvochtigheid te hoog (" + String(avgHum, 1) + "%)";
  } 
  else if (tempTooLow) {
    moldRisk = false;
    moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te laag (" + String(avgTemp, 1) + "°C)";
  }
  else if (tempTooHigh) {
    moldRisk = false;
    String cause = lampsAreOn ? "door kweeklampen" : "door externe warmte";
    moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te hoog " + cause + " (" + String(avgTemp, 1) + "°C)";
  }
  else {
    moldRisk = false;
    moldReasonText = "✅ Ideaal kiemklimaat";
  }

  // LED Status
  if (isClimateCrash || tempTooHigh) {
    statusLed.setColor(255, 0, 0); 
  } 
  else if (vpdTooDry) {
    statusLed.setColor(255, 100, 0); 
    static unsigned long lastDryAlert = 0;
    if (millis() - lastDryAlert > 600000 && ENABLE_TELEGRAM && isConnected) {
      lastDryAlert = millis();
      sendTelegramAlert("🔥 *ALARM DE KLEINE KAS:* " + moldReasonText);
    }
  }
  else if (vpdTooWet || condensRisk || humHigh) {
    statusLed.setColor(255, 128, 0); 
  }
  else {
    statusLed.setColor(0, 0, 0); 
  }

  if (isClimateCrash) vpdStatusText = " `(🚨 NOODSTOP)`";
  else if (vpdTooWet) vpdStatusText = " `(⚠️ Te klam)`";
  else if (vpdTooDry) vpdStatusText = " `(🔥 Te droog)`";
  else vpdStatusText = " `(✅ Optimaal)`";

  if (condensRisk) dpMarginStatusText = " `(⚠️ Risico)`";
  else dpMarginStatusText = " `(✅ Veilig)`";

  if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum)) {
    float correctedIndoorTemp = indoorSmoothedTemp + INDOOR_TEMP_OFFSET;
    float correctedIndoorHum = indoorSmoothedHum + INDOOR_HUM_OFFSET;
    indoorDewPoint = calcDewPoint(correctedIndoorTemp, correctedIndoorHum);
    indoorVpd = calcVPD(correctedIndoorTemp, correctedIndoorHum);
    indoorDpMargin = correctedIndoorTemp - indoorDewPoint;
  }

  if (!isnan(outdoorTemp) && !isnan(outdoorHumidity)) {
    outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
    outdoorVpd = calcVPD(outdoorTemp, outdoorHumidity);
    outdoorDpMargin = outdoorTemp - outdoorDewPoint; 
  }
}

// =========================================================================
// 7. PROACTIEVE SNELHEIDSCONTROLE (Velocity)
// =========================================================================
void checkClimateVelocity(float currentHum) {
  if (millis() - lastVelocityCheckTime >= 60000 || lastVelocityCheckTime == 0) {
    if (previousHumForVelocity != -999.0) {
      humidityVelocity = currentHum - previousHumForVelocity; 
      
      if (humidityVelocity >= 3.0 && currentHum > 60.0) {
        logToSyslogAndSerialPrintf("[ALERT] Snelle vochtigheidstoename in kas: +%.1f%%/min", humidityVelocity);
        kasAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
        kasAdviceReason = "Kasvochtigheid stijgt explosief (+" + String(humidityVelocity, 1) + "%/min); preventieve circulatie gestart.";
      }
    }
    previousHumForVelocity = currentHum;
    lastVelocityCheckTime = millis();
  }
}

// =========================================================================
// 8. VENTILATIE- EN WONING-ADVIESLOGICA MET GELAAGDE FAN-STURING
// =========================================================================
void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;

  if (isKasSleeping) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "💤 De kleine kas is in slaapstand. Klimaatregeling gepauzeerd.";
    fanExt1Speed = 0;
    fanExt2Speed = 0;
    fanIntSpeed = 0;
    return;
  }
  
  if (millis() < 180000) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "Systeem is aan het opstarten...";
    return;
  }

  KasVentState rawNewAdvice = KAS_OFF;
  bool isTooWarm       = (inTemp > GREENHOUSE_MAX_TEMP);
  bool isKritischHeet    = (inTemp > GREENHOUSE_CRASH_TEMP); 
  bool isTooWet        = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
  bool isTooDry        = (kasVpd > VPD_MAX_OPTIMAL);
  bool condensRisk     = (kasDpMargin < DP_MARGIN_MIN);
  
  float curIndoorVpd   = !isnan(indoorVpd) ? indoorVpd : 1.0;
  bool indoorCondensRisk  = (indoorDpMargin < DP_MARGIN_MIN);

  // =========================================================================
  // GELAAGDE FAN-BESTURING (Gebruikt nu Config.h offsets)
  // =========================================================================
  float vpdDelta = VPD_MIN_OPTIMAL - kasVpd; 

  int targetFan1 = 0;
  int targetFan2 = 0;
  int targetIntFan = 100; 

  if (isClimateCrash || isKritischHeet || isTooWarm) {
    // ULTRA MAX: Kritieke noodtoestand -> Alles vol gas open!
    rawNewAdvice = GREENHOUSE_VENTILATE;
    targetFan1 = 255; 
    targetFan2 = 255; 
    kasAdviceReason = "🌡️ KRITIEK: Te warm (" + String(inTemp, 1) + "°C). Externe fans draaien ULTRA MAX!";
  }
  else if (isTooWet || condensRisk) {
    rawNewAdvice = GREENHOUSE_VENTILATE;
    
    // Gebruikt VPD_RED_OFFSET uit Config.h i.p.v. losse 0.4
    if (vpdDelta > VPD_RED_OFFSET || condensRisk) {
      // RODE ZONE (Hard Min / Hard Max): Ernstige afwijking
      targetFan1 = 255; 
      float scaleFactor = constrain((vpdDelta - VPD_RED_OFFSET) / VPD_RED_OFFSET, 0.0, 1.0);
      targetFan2 = (int)(150 + (105 * scaleFactor)); 
      
      kasAdviceReason = "🚨 SCHIMMELRISICO (Rood): Ernstig klam/condensrisico. Beide fans grijpen in!";
    } 
    else if (vpdDelta > VPD_ORANGE_OFFSET) {
      // ORANJE ZONE (Soft Min / Soft Max): Lichte afwijking
      targetFan1 = 120; 
      targetFan2 = 0;   
      
      kasAdviceReason = "⚠️ WAARSCHUWING (Oranje): Kas licht klam. Fan 1 voert zachte correctie uit.";
    }
    else {
      targetFan1 = 0;
      targetFan2 = 0;
    }
  }
  else if (isTooDry) {
    // UITDROGINGSRISICO
    if (!indoorCondensRisk && (curIndoorVpd < kasVpd)) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      targetFan1 = 100; 
      targetFan2 = 0;
      kasAdviceReason = "🔥 Kas te droog, woninglucht wordt subtiel benut ter correctie.";
    } else {
      rawNewAdvice = KAS_OFF;
      targetFan1 = 0;
      targetFan2 = 0;
      kasAdviceReason = "🔥 Kas te droog, kas blijft gesloten om uitdroging te stoppen.";
    }
  }
  else {
    // IDEALE BALANS
    rawNewAdvice = KAS_OFF;
    targetFan1 = 0;
    targetFan2 = 0;
    kasAdviceReason = "✅ Klimaat in de kleine kas is in balans.";
  }

  kasAdvice = applyAntiHuntFilter(rawNewAdvice);

  if (kasAdvice == KAS_OFF && !isTestModeActive) {
    fanExt1Speed = 0;
    fanExt2Speed = 0;
  } else if (!isTestModeActive) {
    fanExt1Speed = targetFan1;
    fanExt2Speed = targetFan2;
  }

  if (kasAdvice != previousKasAdvice) {
    previousKasAdvice = kasAdvice;
    logToSyslogAndSerialPrintf("[ADVICE] Nieuw advies: %s", kasAdviceReason.c_str());
  }
}

// =========================================================================
// 8B. VERWARMINGSLOGICA (Warmtemat)
// =========================================================================
void updateHeatingAdvice(float inTemp, float outTemp) {
  if (isnan(inTemp) || isTestModeActive || isKasSleeping) return; 
  (void)outTemp;

  if (inTemp > 22.0) {
    if (isHeatMatRecommended) {
      logToSerial("[HEATING] Kas temperatuur boven 22°C, warmtemat uitgeschakeld.");
    }
    isHeatMatRecommended = false;
    heatMatLowStartTime = 0;
    heatMatHighStartTime = 0;
    return;
  }

  if (inTemp < HEAT_MAT_TEMP_LOW) {
    heatMatHighStartTime = 0; 
    if (!isHeatMatRecommended) { 
      if (heatMatLowStartTime == 0) heatMatLowStartTime = millis(); 
      else if (millis() - heatMatLowStartTime >= HEAT_MAT_DELAY) {
        logToSyslogAndSerialPrintf("[HEATING] Kas temperatuur te laag (%.1f°C), warmtemat AANbevolen.", inTemp);
        sendTelegramAlert("❗ *Temperatuur kas te laag (" + String(inTemp, 1) + "°C)*\nWarmtemat AANbevolen.");
        isHeatMatRecommended = true; 
        heatMatLowStartTime = 0;     
      }
    }
  } else if (inTemp > HEAT_MAT_TEMP_HIGH) {
    heatMatLowStartTime = 0; 
    if (isHeatMatRecommended) { 
      if (heatMatHighStartTime == 0) heatMatHighStartTime = millis(); 
      else if (millis() - heatMatHighStartTime >= HEAT_MAT_DELAY) {
        logToSyslogAndSerialPrintf("[HEATING] Kas temperatuur OK (%.1f°C), warmtemat UITgeschakeld.", inTemp);
        sendTelegramAlert("☀️ *Temperatuur kas OK (" + String(inTemp, 1) + "°C)*\nWarmtemat UITgeschakeld.");
        isHeatMatRecommended = false; 
        heatMatHighStartTime = 0;     
      }
    }
  } else {
    heatMatLowStartTime = 0;
    heatMatHighStartTime = 0;
  }
}

// =========================================================================
// 9. HISTORIE & STATISTIEKEN
// =========================================================================
void updateMoldRiskHistory(bool currentRisk) {
  if (millis() - lastMoldSampleTime >= 60000 || lastMoldSampleTime == 0) {
    lastMoldSampleTime = millis();
    moldHistory[moldSampleIndex] = currentRisk;
    moldSampleIndex = (moldSampleIndex + 1) % MOLD_SAMPLES;
    if (moldSampleIndex == 0) moldHistoryFilled = true;

    int positiveCount = 0;
    int total = moldHistoryFilled ? MOLD_SAMPLES : moldSampleIndex;
    for (int i = 0; i < total; i++) {
      if (moldHistory[i]) positiveCount++;
    }

    bool filteredMoldRisk = (positiveCount > (total / 2));
    if (filteredMoldRisk != previousMoldRisk) {
      previousMoldRisk = filteredMoldRisk;
      logToSyslogAndSerial(filteredMoldRisk ? "[MOLD ALERT] Schimmelrisico gedetecteerd!" : "[MOLD RECOVERY] Schimmelrisico geweken.");
    }
  }
}

void checkKasTrends(float currentTemp, float currentHum, float currentBaro) {
  if (pastBaro < 0) {
    pastBaro = currentBaro;
    lastTrendSample = millis();
    return;
  }
  (void)currentTemp; 
  (void)currentHum;

  if (millis() - lastTrendSample > 1800000) {
    float baroDiff = currentBaro - pastBaro;
    if (baroDiff >= 0.2) baroTrendArrow = "^";
    else if (baroDiff <= -0.2) baroTrendArrow = "v";
    else baroTrendArrow = "=";

    pastBaro = currentBaro;
    lastTrendSample = millis();
  }
}

void checkIndoorTrends() {}

void updateHighLow(float currentTemp) {
  if (isnan(currentTemp)) return;
  if (millis() - lastHighLowReset > 86400000 || kasTempHigh == -999.0) {
    kasTempHigh = currentTemp;
    kasTempLow = currentTemp;
    lastHighLowReset = millis();
  } else {
    if (currentTemp > kasTempHigh) kasTempHigh = currentTemp;
    if (currentTemp < kasTempLow) kasTempLow = currentTemp;
  }
}

void updateHumidityHighLow(float kasHum, float indoorHum, float outHum) {
  static unsigned long lastHumReset = 0;
  
  if (millis() - lastHumReset > 86400000 || kasLowHum > 200.0) {
    if (!isnan(kasHum)) { kasLowHum = kasHum; kasHighHum = kasHum; }
    if (!isnan(indoorHum)) { indoorLowHum = indoorHum; indoorHighHum = indoorHum; }
    if (!isnan(outHum)) { outdoorLowHum = outHum; outdoorHighHum = outHum; }
    lastHumReset = millis();
    return;
  }

  if (!isnan(kasHum)) {
    if (kasHum < kasLowHum) kasLowHum = kasHum;
    if (kasHum > kasHighHum) kasHighHum = kasHum;
  }
  if (!isnan(indoorHum)) {
    if (indoorHum < indoorLowHum) indoorLowHum = indoorHum;
    if (indoorHum > indoorHighHum) indoorHighHum = indoorHum;
  }
  if (!isnan(outHum)) {
    if (outHum < outdoorLowHum) outdoorLowHum = outHum;
    if (outHum > outdoorHighHum) outdoorHighHum = outHum;
  }
}

// =========================================================================
// 10. TESTMODUS TIMER & AUTOMATISCH HERSTEL
// =========================================================================
void handleTestModeTimeout() {
  if (!isTestModeActive) return;

  if (millis() - testModeStartTime >= currentTestDuration) {
    isTestModeActive = false;
    logToSyslogAndSerial("[TEST] Testmodus automatisch afgelopen. Systeem terug naar automatische regeling.");
    sendTelegramAlert("🧪 *Testmodus afgelopen.*\nSysteem draait weer volledig automatisch op basis van sensoren.");
  }
}