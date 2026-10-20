/**
 * @file ClimateLogic.cpp
 * @brief Zuivere klimaatlogica en advies-evaluatie (stabiele basis).
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"
#include "LedManager.h"

extern LedManager statusLed;
extern bool isClimateCrash; 

static unsigned long lastAlertTime = millis(); 

bool isSystemReady() {
  if (currentLuxValue > 1.0) {
    return (millis() >= 5000); 
  }
  return (millis() >= SYSTEM_STARTUP_DELAY);
}

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

void evaluateClimateState(float kasTemp, float kasHum, float indoorTemp, float indoorHum) {
  if (isnan(kasTemp) || isnan(kasHum)) return;

  if (!isSystemReady()) {
    moldRisk = false;
    moldReasonText = "⏳ Systeem kalibreert / leert...";
    vpdStatusText = " `(⏳ Opstarten)`";
    dpMarginStatusText = " `(⏳ Kalibreren)`";
    return;
  }

  float tempC = kasTemp;
  float hum = kasHum;

  kasSmoothedTemp = tempC;
  kasSmoothedHum  = hum;
  displayedTemp   = tempC; 

  kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
  kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
  kasDpMargin = kasSmoothedTemp - kasDewPoint;

  checkClimateCrash(kasSmoothedTemp);

  bool vpdTooWet   = (kasVpd < VPD_MIN_OPTIMAL);
  bool vpdTooDry   = (kasVpd > VPD_MAX_OPTIMAL);
  bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
  bool humHigh     = (kasSmoothedHum >= HUM_MOLD_THRESHOLD);
  bool tempTooLow  = (kasSmoothedTemp < HEAT_MAT_TEMP_LOW);
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
    String cause = lampsAreOn ? "door lampwarmte" : "door omgevingswarmte/droogte";
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
    String cause = lampsAreOn ? "door verlichting" : "door externe warmte";
    moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te hoog " + cause + " (" + String(kasSmoothedTemp, 1) + "°C)";
  }
  else {
    moldRisk = false;
    moldReasonText = "✅ Ideaal kiemklimaat";
  }

  // LED-indicatie blijft actief voor lokale status, maar het mens-alarmeergedrag via Telegram is hierbij volledig verwijderd.
  if (isClimateCrash || tempTooHigh) {
    statusLed.setColor(255, 0, 0); 
  } 
  else if (vpdTooDry || vpdTooWet || condensRisk) {
    statusLed.setColor(255, 100, 0); 
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
    if (outdoorHumidity <= 0.0 || outdoorHumidity > 100.0 || outdoorTemp <= -900.0) {
      outdoorDewPoint = -999.0;
      outdoorVpd = -999.0;
      outdoorDpMargin = -999.0;
    } else {
      outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
      outdoorVpd = calcVPD(outdoorTemp, outdoorHumidity);
      outdoorDpMargin = outdoorTemp - outdoorDewPoint; 
    }
  }
}

void checkClimateVelocity(float currentHum) {
  if (millis() - lastVelocityCheckTime >= 60000 || lastVelocityCheckTime == 0) {
    if (previousHumForVelocity != -999.0) {
      humidityVelocity = currentHum - previousHumForVelocity; 
      if (humidityVelocity >= 3.0 && currentHum > 60.0) {
        logToSyslogAndSerialPrintf("[ALERT] Snelle vochtigheidstoename in kas: +%.1f%%/min", humidityVelocity);
        kasAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
      }
    }
    previousHumForVelocity = currentHum;
    lastVelocityCheckTime = millis();
  }
}

bool homeVentActive = false;

void updateHomeVentilationAdvice(float inTemp, float inHum, float outTemp, float outHum) {
  if (isnan(inHum) || isnan(outHum) || isnan(inTemp) || isnan(outTemp)) return;

  bool tempSafeForHome = (outTemp > 1.0f && outTemp < 35.0f); 
  float homeOutdoorDewPoint = calcDewPoint(outTemp, outHum);
  float homeIndoorDewPoint  = calcDewPoint(inTemp, inHum);

  bool outdoorCanDehumidifyHome = (homeOutdoorDewPoint < homeIndoorDewPoint);
  bool indoorNeedsVentilation = (inHum > 55.0f) || (indoorVpd > VPD_MAX_OPTIMAL) || (indoorVpd < VPD_MIN_OPTIMAL);

  homeVentActive = (indoorNeedsVentilation && outdoorCanDehumidifyHome && tempSafeForHome);
}

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

  KasVentState rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
  kasAdviceReason = "🔄 Interne circulatie actief (standaard kas-balans).";

  bool isTooWarm    = (inTemp > GREENHOUSE_MAX_TEMP);
  bool isKritischHeet = (inTemp > GREENHOUSE_CRASH_TEMP); 
  bool isTooWet     = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
  bool isTooDry     = (kasVpd > VPD_MAX_OPTIMAL); 
  bool condensRisk  = (kasDpMargin < DP_MARGIN_MIN);
  
  bool indoorCanHelpDehumidify = (!isnan(indoorVpd) && indoorVpd > kasVpd && indoorVpd < VPD_MAX_OPTIMAL);
  bool indoorCanHelpHumidify   = (!isnan(indoorVpd) && indoorVpd < kasVpd && indoorVpd > VPD_MIN_OPTIMAL);

  if (isClimateCrash || isKritischHeet || isTooWarm) {
    if (indoorCanHelpDehumidify || inTemp > (GREENHOUSE_MAX_TEMP + 1.0)) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = "🌡️ Te warm (" + String(inTemp, 1) + "°C). Externe ventilatie actief met woninglucht.";
    } else {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🌡️ Te warm, maar woninglucht biedt geen verbetering. Interne circulatie aangehouden.";
    }
  }
  else if (isTooWet || condensRisk) {
    if (indoorCanHelpDehumidify) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = "💧 Kas te klam; woninglucht is gunstiger en wordt ingezet voor ontvochtiging.";
    } else {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🌧️ Kas te klam, maar woninglucht helpt niet (of is te vochtig). Kas blijft dicht.";
    }
  }
  else if (isTooDry || lampsAreOn) {
    if (indoorCanHelpHumidify && kasVpd > (VPD_MAX_OPTIMAL + 0.1)) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = lampsAreOn 
        ? "💡 Lampen aan & kas te droog; woninglucht wordt ingezet om VPD te balanceren."
        : "🔥 Kas te droog; woninglucht biedt vochtcorrectie.";
    } else {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = lampsAreOn 
        ? "💡 Lampen aan & kas warm/droog; interne circulatie handhaaft het microklimaat."
        : "🔥 Kas te droog, maar woninglucht helpt niet. Kas blijft dicht.";
    }
  }
  else {
    rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "🔄 Interne circulatie actief (standaard kas-balans).";
  }

  static KasVentState candidateAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
  static int persistenceCounter = 0;
  const int REQUIRED_PERSISTENT_CHECKS = 5;

  if (rawNewAdvice != lastAppliedKasAdvice) {
    if (rawNewAdvice == candidateAdvice) {
      persistenceCounter++;
      if (persistenceCounter >= REQUIRED_PERSISTENT_CHECKS) {
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

void updateHeatingAdvice(float inTemp, float outTemp) {
  if (isnan(inTemp) || isTestModeActive || isKasSleeping) return; 
  (void)outTemp;
  if (inTemp > 22.0) {
    if (isHeatMatRecommended) logToSerial("[HEATING] Kas temperatuur boven 22°C, warmtemat uitgeschakeld.");
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
        isHeatMatRecommended = false; 
        heatMatHighStartTime = 0;     
      }
    }
  } else {
    heatMatLowStartTime = 0;
    heatMatHighStartTime = 0;
  }
}

void updateMoldRiskHistory(bool currentRisk) {
  if (millis() - lastMoldSampleTime >= 60000 || lastMoldSampleTime == 0) {
    lastMoldSampleTime = millis();
    moldHistory[moldSampleIndex] = currentRisk;
    moldSampleIndex = (moldSampleIndex + 1) % MOLD_SAMPLES;
    if (moldSampleIndex == 0) moldHistoryFilled = true;

    int positiveCount = 0;
    int total = moldHistoryFilled ? MOLD_SAMPLES : moldSampleIndex;
    for (int i = 0; i < total; i++) { if (moldHistory[i]) positiveCount++; }

    bool filteredMoldRisk = (positiveCount > (total / 2));
    if (filteredMoldRisk != previousMoldRisk) {
      previousMoldRisk = filteredMoldRisk;
      logToSyslogAndSerial(filteredMoldRisk ? "[MOLD ALERT] Schimmelrisico gedetecteerd!" : "[MOLD RECOVERY] Schimmelrisico geweken.");
    }
  }
}

void checkKasTrends(float currentTemp, float currentHum, float currentBaro) {
  if (pastBaro < 0) { pastBaro = currentBaro; lastTrendSample = millis(); return; }
  (void)currentTemp; (void)currentHum;
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
void updateHighLow(float currentTemp) { if (isnan(currentTemp)) return; (void)currentTemp; }
void updateHumidityHighLow(float kasHum, float indoorHum, float outHum) { (void)kasHum; (void)indoorHum; (void)outHum; }
void handleTestModeTimeout() { if (!isTestModeActive) return; if (millis() - testModeStartTime >= currentTestDuration) { isTestModeActive = false; } }