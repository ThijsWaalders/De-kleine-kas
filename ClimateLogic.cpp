/**
 * @file ClimateLogic.cpp
 * @brief Implementatie van klimaatberekeningen, sturingslogica en robuuste grenzen met anti-hunt ventilatorbeveiliging voor ESP32-S3.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"

// =========================================================================
// 1. ZELFLERENDE / ADAPTIEVE PARAMETERS (VPD)
// =========================================================================
float adaptiveVpdMinOffset = 0.0; 
float adaptiveVpdMaxOffset = 0.0; 
unsigned long lastAdaptationTime = 0;

// Vaste correcties
float kasTempOffset = 0.0; 
float kasHumOffset = 0.0;  

// =========================================================================
// 2. ANTI-HUNT VARIABELEN VOOR VENTILATOR
// =========================================================================
unsigned long fanStateChangeTime = 0;
const unsigned long MIN_FAN_RUN_TIME = 180000; // Minimaal 3 minuten aan/uit blijven
KasVentState lastAppliedKasAdvice = OFF;

// =========================================================================
// 3. GLOBALE KLIMAAT & MARGE VARIABELEN
// =========================================================================
float kasDpMargin = 0.0;
float indoorDpMargin = 0.0;
String kasDpIcon = "🟢 (Veilig)";
String indoorDpIcon = "🟢 (Veilig)";
String indoorVpdIcon = "🟢";

// =========================================================================
// 4. HULPFUNCTIES (AI Leren & Anti-Hunt)
// =========================================================================

/**
 * @brief Laat het systeem autonoom leren en drempelwaarden bijstellen op basis van historisch gedrag.
 */
void updateSelfLearningParameters(float currentVpd, bool currentMoldRisk) {
  unsigned long currentMillis = millis();
  
  if (currentMillis - lastAdaptationTime > 21600000 || lastAdaptationTime == 0) {
    lastAdaptationTime = currentMillis;

    if (!currentMoldRisk) {
      if (currentVpd < (VPD_MIN_OPTIMAL + adaptiveVpdMinOffset) && currentVpd > 0.2f) {
        adaptiveVpdMinOffset -= 0.02f;
        adaptiveVpdMinOffset = max(adaptiveVpdMinOffset, -0.2f);
        logToSerialPrintf("[AI LEARNING] Adaptieve VPD-ondergrens bijgesteld naar: %.2f", VPD_MIN_OPTIMAL + adaptiveVpdMinOffset);
      }
    }
  }
}

/**
 * @brief Beveiliging tegen "dansen" van de ventilator door sensorstoring of windfluctuaties.
 */
KasVentState applyAntiHuntFilter(KasVentState rawNewAdvice) {
  unsigned long currentMillis = millis();

  if (rawNewAdvice != lastAppliedKasAdvice) {
    if (currentMillis - fanStateChangeTime < MIN_FAN_RUN_TIME) {
      return lastAppliedKasAdvice; // Negeer de wissel tijdelijk om klapperen te voorkomen
    }
    lastAppliedKasAdvice = rawNewAdvice;
    fanStateChangeTime = currentMillis;
    logToSerial("[ANTI-HUNT] Nieuwe ventilatorstatus geaccepteerd.");
  }

  return lastAppliedKasAdvice;
}

// =========================================================================
// 5. HOOFD KLIMAAT- & SCHIMMELDIAGNOSE (4 Prioriteiten)
// =========================================================================

/**
 * @brief Evalueert het kas-klimaat op basis van vaste prioriteiten en berekent marges/iconen.
 */
void evaluateClimateState(float tempC, float hum) {
  if (isnan(tempC) || isnan(hum)) return;

  // Eerste 3 minuten: Systeem is aan het opstarten / kalibreren / leren
  if (millis() < 180000) {
    moldRisk = false;
    moldReasonText = "⏳ Systeem kalibreert / leert...";
    vpdStatusText = " `(⏳ Opstarten)`";
    dpMarginStatusText = " `(⏳ Kalibreren)`";
    return;
  }

  float correctedTemp = tempC + kasTempOffset;
  float correctedHum = hum + kasHumOffset;

  kasDewPoint = calcDewPoint(correctedTemp, correctedHum);
  kasVpd = calcVPD(correctedTemp, correctedHum);
  
  kasDpMargin = correctedTemp - kasDewPoint;
  if (kasDpMargin < 3.0) kasDpIcon = "🔴 (Kritiek)";
  else if (kasDpMargin < 5.0) kasDpIcon = "🟡 (Let op)";
  else kasDpIcon = "🟢 (Veilig)";

  float effectiveVpdMin = VPD_MIN_OPTIMAL + adaptiveVpdMinOffset;
  float effectiveVpdMax = VPD_MAX_OPTIMAL + adaptiveVpdMaxOffset;

  bool vpdTooWet = (kasVpd < effectiveVpdMin);
  bool vpdTooDry = (kasVpd > effectiveVpdMax);
  bool condensRisk = (kasDpMargin < (DP_MARGIN_MIN - 0.5));
  bool humHigh = (correctedHum >= (HUM_MOLD_THRESHOLD + 2.0));
  bool tempTooLow = (correctedTemp < HEAT_MAT_TEMP_LOW);
  bool tempTooHigh = (correctedTemp > GREENHOUSE_MAX_TEMP);

  if (vpdTooWet) {
    moldRisk = true;
    moldReasonText = "⚠️ RISICO: VPD te klam (adaptief: " + String(kasVpd, 2) + " kPa)";
  }
  else if (vpdTooDry) {
    moldRisk = true;
    moldReasonText = "⚠️ RISICO: VPD te droog (" + String(kasVpd, 2) + " kPa)";
  }
  else if (condensRisk) {
    moldRisk = true;
    moldReasonText = "⚠️ KRITIEK: Condensgevaar / Dauwpuntmarge te krap (" + String(kasDpMargin, 1) + "°C)";
  } 
  else if (humHigh) {
    moldRisk = true;
    moldReasonText = "⚠️ RISICO: Luchtvochtigheid te hoog (" + String(correctedHum, 1) + "%)";
  } 
  else if (tempTooLow) {
    moldRisk = true;
    moldReasonText = "⚠️ WAARSCHUWING: Kas temperatuur te laag (" + String(correctedTemp, 1) + "°C)";
  }
  else if (tempTooHigh) {
    moldRisk = true;
    moldReasonText = "⚠️ WAARSCHUWING: Kas temperatuur te hoog (" + String(correctedTemp, 1) + "°C)";
  }
  else {
    moldRisk = false;
    moldReasonText = "✅ Geen risico (Ideaal kiemklimaat)";
  }

  updateSelfLearningParameters(kasVpd, moldRisk);

  if (kasVpd < effectiveVpdMin) vpdStatusText = " `(⚠️ Te klam)`";
  else if (kasVpd > effectiveVpdMax) vpdStatusText = " `(🔥 Te droog)`";
  else vpdStatusText = " `(✅ Optimaal)`";

  if (kasDpMargin < DP_MARGIN_MIN) dpMarginStatusText = " `(⚠️ Risico)`";
  else dpMarginStatusText = " `(✅ Veilig)`";
}

// =========================================================================
// 6. PROACTIEVE SNELHEIDSCONTROLE (Snel stijgende vochtigheid)
// =========================================================================
void checkClimateVelocity(float currentHum) {
  if (millis() < 180000) return;

  if (millis() - lastVelocityCheckTime >= 60000 || lastVelocityCheckTime == 0) {
    if (previousHumForVelocity != -999.0) {
      humidityVelocity = currentHum - previousHumForVelocity; 
      
      if (humidityVelocity >= 3.0 && currentHum > 60.0) {
        logToSyslogAndSerialPrintf("[ALERT] Snelle vochtigheidstoename in kas: +%.1f%%/min", humidityVelocity);
        sendTelegramAlert("⚠️ *PROACTIEF ALERT:* Kasvochtigheid stijgt erg snel (+" + String(humidityVelocity, 1) + "%/min)! Circulatie preventief gestart.");
        kasAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
      }
    }
    previousHumForVelocity = currentHum;
    lastVelocityCheckTime = millis();
  }
}

// =========================================================================
// 7. VENTILATIE- EN VERWARMINGSLOGICA
// =========================================================================
void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;

  if (millis() < 180000) {
    kasAdvice = OFF;
    kasAdviceReason = "Systeem is aan het opstarten / kalibreren / leren...";
    return;
  }

  float tempDelta = inTemp - indoorTemp;          
  float dewPointDelta = inDp - indoorDewPoint;    

  KasVentState rawNewAdvice = kasAdvice;
  float effectiveVpdMin = VPD_MIN_OPTIMAL + adaptiveVpdMinOffset;

  const float MAX_TEMP_DIFF = 3.5; 

  bool isTooWarmInKas = (inTemp > GREENHOUSE_MAX_TEMP) || (tempDelta > MAX_TEMP_DIFF);
  bool canCoolWithRoom = (indoorTemp < inTemp); 
  bool isTooColdInKas = (inTemp < HEAT_MAT_TEMP_LOW); 

  if (isTooWarmInKas && canCoolWithRoom) {
    rawNewAdvice = GREENHOUSE_VENTILATE;
    kasAdviceReason = "Kas overschrijdt max temp (" + String(GREENHOUSE_MAX_TEMP, 1) + "°C) en kamer is koeler; afvoeren.";
  } 
  else if (isTooWarmInKas && !canCoolWithRoom) {
    if (kasVpd < effectiveVpdMin || inHum >= 65.0 || moldRisk) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = "Kas te warm maar kamer biedt geen koeling; geforceerd ventileren op basis van vocht/VPD.";
    } else {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "Kas warm maar kamer is warmer; intern circuleren om microklimaat te breken.";
    }
  }
  else if (isTooColdInKas && indoorTemp > inTemp) {
    rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "Kas te koud; intern circuleren om warmte uit de woning te benutten.";
  }
  else if (kasVpd < effectiveVpdMin || inHum >= 65.0 || moldRisk || (tempDelta > 3.0 && dewPointDelta > 2.0)) {
    rawNewAdvice = GREENHOUSE_VENTILATE; 
    kasAdviceReason = "Kas is te klam of schimmelrisico gedetecteerd; afvoeren naar kamer.";
  } 
  else if (indoorHum < 40.0 && inHum < 60.0) {
    rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
    kasAdviceReason = "Woninglucht is erg droog; intern circuleren om uitdroging van kiemgroenten te voorkomen.";
  }
  else if (inHum >= 55.0 || kasVpd < (effectiveVpdMin + 0.1)) {
    rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
    kasAdviceReason = "Lichte vochtophoping in kas; intern circuleren om microklimaat te breken.";
  } 
  else {
    rawNewAdvice = OFF; 
    kasAdviceReason = "Klimaat in de kas en t.o.v. de woning is volledig in balans.";
  }

  kasAdvice = applyAntiHuntFilter(rawNewAdvice);

  if (kasAdvice != previousKasAdvice) {
    previousKasAdvice = kasAdvice;
    if (kasAdvice == GREENHOUSE_VENTILATE) {
      logToSyslogAndSerialPrintf("[ADVICE] Kas advies gewijzigd: Ventileren naar kamer. Reden: %s", kasAdviceReason.c_str());
      sendTelegramAlert("🪴 *AUTOMATISCH: Ventilatie naar kamer gestart*\n" + kasAdviceReason);
    } else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
      logToSyslogAndSerialPrintf("[ADVICE] Kas advies gewijzigd: Intern circuleren. Reden: %s", kasAdviceReason.c_str());
      sendTelegramAlert("🌀 *AUTOMATISCH: Intern circuleren gestart*\n" + kasAdviceReason);
    } else if (kasAdvice == OFF) {
      logToSyslogAndSerial("[ADVICE] Kas advies gewijzigd: Geen actie. Systeem in balans.");
      sendTelegramAlert("✅ *AUTOMATISCH: Systeem in balans (Rust)*\nDe kas draait stabiel.");
    }
  }
}

void updateHeatingAdvice(float inTemp, float outTemp) {
  if (isnan(inTemp) || isTestModeActive) return; 

  if (inTemp > 22.0) {
    if (isHeatMatRecommended) {
      logToSerial("[HEATING] Kas temperatuur boven 22°C, warmtemat veiligheidshalve uitgeschakeld.");
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
        sendTelegramAlert("❗ *Temperatuur kas te laag (" + String(inTemp, 1) + "°C)*\nSchakel de verwarmingsmat AAN!");
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
        sendTelegramAlert("☀️ *Temperatuur kas OK (" + String(inTemp, 1) + "°C)*\nSchakel de warmtemat UIT.");
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
// 8. HISTORIE, TREDES & STATISTIEKEN (High/Low)
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
      sendTelegramAlert(filteredMoldRisk ? "⚠️ *WAARSCHUWING:* Schimmelrisico gedetecteerd in de kas!" : "✅ *HERSTEL:* Schimmelrisico in de kas is geweken.");
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

void checkRoomTrends() {}

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
  
  if (millis() - lastHumReset > 86400000 || kasLowHum == 100.0) {
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
    if (indoorHum > indoorHighHum) indoorHighHum = indoorHighHum;
  }
  if (!isnan(outHum)) {
    if (outHum < outdoorLowHum) outdoorLowHum = outHum;
    if (outHum > outdoorHighHum) outdoorHighHum = outHum;
  }
}