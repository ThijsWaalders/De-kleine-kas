/**
 * @file ClimateLogic.cpp
 * @brief Implementatie van robuuste klimaatberekeningen voor kiemgroenten (ESP32-S3) met gelaagde fan-sturing.
 *        Maakt strikt gebruik van de Single Source of Truth (Config.h) en directe sensorwaarden.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"
#include "LedManager.h"

extern LedManager statusLed;

// Check of lampen branden, zoja skip kalibratie en start direct
bool isSystemReady();

// Globale vlag voor de noodtoestand
bool isClimateCrash = false; 

// =========================================================================
// OPSTART-BYPASS HULPFUNCTIE (Lampen aan = direct starten)
// =========================================================================
bool isSystemReady() {
  return (millis() >= SYSTEM_STARTUP_DELAY || currentLuxValue > 1.0);
}

// =========================================================================
// ANTI-HUNT VARIABELEN VOOR VENTILATOR
// =========================================================================
unsigned long fanStateChangeTime = 0;
KasVentState lastAppliedKasAdvice = KAS_OFF;

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
// KLIMAAT CRASH BEWAKING
// =========================================================================
void checkClimateCrash(float inTemp) {
  bool criticalTempHigh = (inTemp >= (GREENHOUSE_CRASH_TEMP + 2.0)); 
  bool safeTempRecovery = (inTemp <= GREENHOUSE_CRASH_TEMP);       

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
// HOOFD KLIMAAT- & SCHIMMELDIAGNOSE (Direct & Realtime zonder trage buffer)
// =========================================================================
void evaluateClimateState(float tempC, float hum) {
  if (isnan(tempC) || isnan(hum)) return;

  if (!isSystemReady()) {
    moldRisk = false;
    moldReasonText = "⏳ Systeem kalibreert / leert...";
    vpdStatusText = " `(⏳ Opstarten)`";
    dpMarginStatusText = " `(⏳ Kalibreren)`";
    return;
  }

  // Direct de pure, actuele waarden gebruiken zonder trage gemiddelden
  kasSmoothedTemp = tempC;
  kasSmoothedHum  = hum;
  displayedTemp   = tempC; 

  kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
  kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
  kasDpMargin = kasSmoothedTemp - kasDewPoint;

  checkClimateCrash(kasSmoothedTemp);

  // --- ADAPTIEVE LAMP-TRANSITIE BEWAKKING ---
  // Zodra de lampen geschakeld zijn, controleren we of het klimaat al is gestabiliseerd.
  if (isLampTransitionActive) {
    // Na minimaal 3 minuten kijken of de vochtigheids- en temperatuursnelheid nagenoeg nul is
    if (millis() - lampTransitionStartTime > 180000 && abs(humidityVelocity) < 0.3) {
      isLampTransitionActive = false;
      logToSyslogAndSerial("[PREDICTIVE] Kas is adaptief gestabiliseerd na lamp-transitie.");
    }
    // Harde veiligheidslimiet: maximaal 30 minuten transitiefase voordat we geforceerd stoppen
    else if (millis() - lampTransitionStartTime > 1800000) {
      isLampTransitionActive = false;
    }
  }

  bool vpdTooWet = (kasVpd < VPD_MIN_OPTIMAL);
  bool vpdTooDry = (kasVpd > VPD_MAX_OPTIMAL);
  bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
  bool humHigh = (kasSmoothedHum >= HUM_MOLD_THRESHOLD);
  bool tempTooLow = (kasSmoothedTemp < HEAT_MAT_TEMP_LOW);
  bool tempTooHigh = (kasSmoothedTemp > GREENHOUSE_MAX_TEMP);

  if (isClimateCrash) {
    moldRisk = true;
    moldReasonText = "🚨 NOODTOESTAND: Kas uit de hand gelopen (" + String(kasSmoothedTemp, 1) + "°C)!";
  }
  else if (vpdTooWet) {
    moldRisk = true;
    moldReasonText = "⚠️ SCHIMMELRISICO: Kas te klam (VPD " + String(kasVpd, 2) + " < " + String(VPD_MIN_OPTIMAL, 2) + " kPa)";
  }
  else if (vpdTooDry) {
    moldRisk = false; 
    String cause = lampsAreOn ? "door lamp warmte" : "door omgevingswarmte/droogte";
    moldReasonText = "⚠️ UITDROGINGSRISICO: Kas te droog " + cause + " (VPD " + String(kasVpd, 2) + " kPa)";
  }
  else if (condensRisk) {
    moldRisk = true;
    moldReasonText = "⚠️ KRITIEK: Condensgevaar op zaadjes! Marge: " + String(kasDpMargin, 1) + "°C";
  } 
  else if (humHigh) {
    moldRisk = true;
    moldReasonText = "⚠️ RISICO: Luchtvochtigheid te hoog (" + String(kasSmoothedHum, 1) + "%)";
  } 
  else if (tempTooLow) {
    moldRisk = false;
    moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te laag (" + String(kasSmoothedTemp, 1) + "°C)";
  }
  else if (tempTooHigh) {
    moldRisk = false;
    String cause = lampsAreOn ? "door kweeklampen" : "door externe warmte";
    moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te hoog " + cause + " (" + String(kasSmoothedTemp, 1) + "°C)";
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
    indoorDewPoint = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
    indoorVpd = calcVPD(indoorSmoothedTemp, indoorSmoothedHum);
    indoorDpMargin = indoorSmoothedTemp - indoorDewPoint;
  }

  if (!isnan(outdoorTemp) && !isnan(outdoorHumidity)) {
    outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
    outdoorVpd = calcVPD(outdoorTemp, outdoorHumidity);
    outdoorDpMargin = outdoorTemp - outdoorDewPoint; 
  }
}

// =========================================================================
// PROACTIEVE SNELHEIDSCONTROLE (Velocity)
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

bool homeVentActive = false;

void updateHomeVentilationAdvice(float inTemp, float inHum, float outTemp, float outHum) {
  if (isnan(inHum) || isnan(outHum)) return;

  bool outdoorTooHumid = (outHum > 85.0);       
  bool indoorMoreHumid = (inHum > 62.0);         
  bool tempAcceptable  = (outTemp > 8.0 && outTemp < 32.0); 

  if (indoorMoreHumid && !outdoorTooHumid && tempAcceptable) {
    homeVentActive = true;
  } else {
    homeVentActive = false;
  }
}

// =========================================================================
// VENTILATIE-ADVIESLOGICA MET PERSISTENTIE (Anti-Dancing / Debounce)
// =========================================================================
void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;

  updateHomeVentilationAdvice(inTemp, inHum, outTemp, outHum);

  if (isKasSleeping) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "💤 De kleine kas is in slaapstand. Klimaatregeling gepauzeerd.";
    return;
  }
  
  if (!isSystemReady()) {
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
  
  bool outdoorIsTooKlam = (!isnan(outdoorHumidity) && outdoorHumidity > OUTDOOR_MAX_HUMIDITY_FOR_VENT);
  bool indoorCanHelp   = (!isnan(indoorVpd) && indoorVpd > kasVpd);
  bool indoorCondensRisk = (!isnan(indoorDpMargin) && indoorDpMargin < DP_MARGIN_MIN);

  if (isClimateCrash || isKritischHeet || isTooWarm) {
    rawNewAdvice = GREENHOUSE_VENTILATE;
    kasAdviceReason = "🌡️ KRITIEK: Te warm (" + String(inTemp, 1) + "°C). Externe fans moeten luchten!";
  }
  else if (isTooWet || condensRisk) {
    if (outdoorIsTooKlam) {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🔴 Schimmelrisico, MAAR buitenlucht is te klam (>85% LV). Externe ventilatie geblokkeerd.";
    } 
    else if (!indoorCanHelp && !isnan(indoorVpd)) {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "⚠️ Schimmelrisico in kas, maar woninglucht is te klam. Alleen interne circulatie.";
    } 
    else {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = "🚨 SCHIMMELRISICO: Ernstig klam/condensrisico. Luchtverversing nodig!";
    }
  }
  else if (isTooDry) {
    if (!indoorCondensRisk && !isnan(indoorVpd) && (indoorVpd < kasVpd)) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = "🔥 Kas te droog, woninglucht wordt benut.";
    } else {
      rawNewAdvice = KAS_OFF;
      kasAdviceReason = "🔥 Kas te droog, kas blijft gesloten.";
    }
  }
  else if (isLampTransitionActive && lampsAreOn) {
    rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "💡 Lampen recent ingeschakeld; adaptieve interne circulatie draait tot het klimaat stabiel is.";
  }
  else {
    rawNewAdvice = KAS_OFF;
    kasAdviceReason = "✅ Klimaat in de kleine kas is in balans.";
  }

  // --- PERSISTENTIE & DEBOUNCE LOGICA TEGEN DANSEN ---
  static KasVentState candidateAdvice = KAS_OFF;
  static int persistenceCounter = 0;
  const int REQUIRED_PERSISTENT_CHECKS = 3; // Moet 3 opeenvolgende metingen (9 sec) aanhouden

  if (rawNewAdvice != lastAppliedKasAdvice) {
    if (rawNewAdvice == candidateAdvice) {
      persistenceCounter++;
      if (persistenceCounter >= REQUIRED_PERSISTENT_CHECKS) {
        // Pas na X keer bevestiging accepteren we het nieuwe advies
        kasAdvice = applyAntiHuntFilter(rawNewAdvice);
        persistenceCounter = 0;
      }
    } else {
      candidateAdvice = rawNewAdvice;
      persistenceCounter = 1;
    }
  } else {
    persistenceCounter = 0;
    kasAdvice = applyAntiHuntFilter(rawNewAdvice);
  }

  if (kasAdvice != previousKasAdvice) {
    previousKasAdvice = kasAdvice;
    logToSyslogAndSerialPrintf("[ADVICE] Nieuw advies: %s", kasAdviceReason.c_str());
  }
}
// // =========================================================================
// // VENTILATIE-ADVIESLOGICA WITH ADAPTIVE LAMP TRANSITION ANTICIPATION
// // =========================================================================
// void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
//   if (isnan(inTemp) || isnan(inHum)) return;

//   updateHomeVentilationAdvice(inTemp, inHum, outTemp, outHum);

//   if (isKasSleeping) {
//     kasAdvice = KAS_OFF;
//     kasAdviceReason = "💤 De kleine kas is in slaapstand. Klimaatregeling gepauzeerd.";
//     return;
//   }
  
//   if (!isSystemReady()) {
//     kasAdvice = KAS_OFF;
//     kasAdviceReason = "Systeem is aan het opstarten...";
//     return;
//   }

//   KasVentState rawNewAdvice = KAS_OFF;
//   bool isTooWarm       = (inTemp > GREENHOUSE_MAX_TEMP);
//   bool isKritischHeet    = (inTemp > GREENHOUSE_CRASH_TEMP); 
//   bool isTooWet        = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
//   bool isTooDry        = (kasVpd > VPD_MAX_OPTIMAL);
//   bool condensRisk     = (kasDpMargin < DP_MARGIN_MIN);
  
//   bool outdoorIsTooKlam = (!isnan(outdoorHumidity) && outdoorHumidity > OUTDOOR_MAX_HUMIDITY_FOR_VENT);
//   bool indoorCanHelp   = (!isnan(indoorVpd) && indoorVpd > kasVpd);
//   bool indoorCondensRisk = (!isnan(indoorDpMargin) && indoorDpMargin < DP_MARGIN_MIN);

//   if (isClimateCrash || isKritischHeet || isTooWarm) {
//     rawNewAdvice = GREENHOUSE_VENTILATE;
//     kasAdviceReason = "🌡️ KRITIEK: Te warm (" + String(inTemp, 1) + "°C). Externe fans moeten luchten!";
//   }
//   else if (isTooWet || condensRisk) {
//     if (outdoorIsTooKlam) {
//       rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//       kasAdviceReason = "🔴 Schimmelrisico, MAAR buitenlucht is te klam (>85% LV). Externe ventilatie geblokkeerd.";
//     } 
//     else if (!indoorCanHelp && !isnan(indoorVpd)) {
//       rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//       kasAdviceReason = "⚠️ Schimmelrisico in kas, maar woninglucht is te klam. Alleen interne circulatie.";
//     } 
//     else {
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "🚨 SCHIMMELRISICO: Ernstig klam/condensrisico. Luchtverversing nodig!";
//     }
//   }
//   else if (isTooDry) {
//     if (!indoorCondensRisk && !isnan(indoorVpd) && (indoorVpd < kasVpd)) {
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "🔥 Kas te droog, woninglucht wordt benut.";
//     } else {
//       rawNewAdvice = KAS_OFF;
//       kasAdviceReason = "🔥 Kas te droog, kas blijft gesloten.";
//     }
//   }
//   else if (isLampTransitionActive && lampsAreOn) {
//     // PREDICTIVE CONTROL: Lampen zijn net aan gegaan en het klimaat is nog in transitie. 
//     // We starten preventieve interne circulatie om de opkomende warmte gelijkmatig te verdelen.
//     rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//     kasAdviceReason = "💡 Lampen recent ingeschakeld; adaptieve interne circulatie draait tot het klimaat stabiel is.";
//   }
//   else {
//     rawNewAdvice = KAS_OFF;
//     kasAdviceReason = "✅ Klimaat in de kleine kas is in balans.";
//   }

//   kasAdvice = applyAntiHuntFilter(rawNewAdvice);

//   if (kasAdvice != previousKasAdvice) {
//     previousKasAdvice = kasAdvice;
//     logToSyslogAndSerialPrintf("[ADVICE] Nieuw advies: %s", kasAdviceReason.c_str());
//   }
// }

// =========================================================================
// VERWARMINGSLOGICA (Warmtemat)
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
// HISTORIE & STATISTIEKEN
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

void handleTestModeTimeout() {
  if (!isTestModeActive) return;

  if (millis() - testModeStartTime >= currentTestDuration) {
    isTestModeActive = false;
    logToSyslogAndSerial("[TEST] Testmodus automatisch afgelopen. Systeem terug naar automatische regeling.");
    sendTelegramAlert("🧪 *Testmodus afgelopen.*\nSysteem draait weer volledig automatisch op basis van sensoren.");
  }
}