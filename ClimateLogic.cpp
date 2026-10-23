/**
 * @file ClimateLogic.cpp
 * @brief Zuivere klimaatlogica, fysische formules (VPD/Dauwpunt) en status-evaluatie.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"
#include "LedManager.h"

extern LedManager statusLed;
extern bool isClimateCrash; 

bool isSystemReady() {
  if (currentLuxValue > SYSTEM_READY_LUX_THRESHOLD) {
    return (millis() >= SYSTEM_READY_SHORT_DELAY_MS); 
  }
  return (millis() >= SYSTEM_STARTUP_DELAY);
}

void checkClimateCrash(float inTemp) {
  bool criticalTempHigh = (inTemp >= (GREENHOUSE_CRASH_TEMP + GREENHOUSE_CRASH_OFFSET)); 
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

  kasSmoothedTemp = kasTemp;
  kasSmoothedHum  = kasHum;
  displayedTemp   = kasTemp; 

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

  // LED indicatie voor de kas
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

void updateHeatingAdvice(float inTemp, float outTemp) {
  if (isnan(inTemp) || isTestModeActive || isKasSleeping) return; 
  (void)outTemp;
  if (inTemp > HEAT_MAT_DISABLE_TEMP) {
    if (isHeatMatRecommended) logToSerial("[HEATING] Kas temperatuur boven grenswaarde, warmtemat uitgeschakeld.");
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

void checkClimateVelocity(float currentHum) {
  if (millis() - lastVelocityCheckTime >= CLIMATE_CHECK_INTERVAL_MS || lastVelocityCheckTime == 0) {
    if (previousHumForVelocity != -999.0) {
      humidityVelocity = currentHum - previousHumForVelocity; 
      if (humidityVelocity >= HUMIDITY_VELOCITY_THRESHOLD && currentHum > HUMIDITY_VELOCITY_MIN_HUM) {
        logToSyslogAndSerialPrintf("[ALERT] Snelle vochtigheidstoename in kas: +%.1f%%/min", humidityVelocity);
        kasAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
      }
    }
    previousHumForVelocity = currentHum;
    lastVelocityCheckTime = millis();
  }
}

void updateMoldRiskHistory(bool currentRisk) {
  if (millis() - lastMoldSampleTime >= CLIMATE_CHECK_INTERVAL_MS || lastMoldSampleTime == 0) {
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
  if (millis() - lastTrendSample > CLIMATE_CHECK_INTERVAL_MS) {
    float baroDiff = currentBaro - pastBaro;
    if (baroDiff >= BARO_TREND_THRESHOLD) baroTrendArrow = "^";
    else if (baroDiff <= -BARO_TREND_THRESHOLD) baroTrendArrow = "v";
    else baroTrendArrow = "=";
    pastBaro = currentBaro;
    lastTrendSample = millis();
  }
}

void updateHighLow(float currentTemp) { 
  if (isnan(currentTemp)) return; 
  (void)currentTemp; 
}

void updateHumidityHighLow(float kasHum, float indoorHum, float outHum) { 
  (void)kasHum; (void)indoorHum; (void)outHum; 
}

// /**
//  * @file ClimateLogic.cpp
//  * @brief Zuivere klimaatlogica, fysische formules (VPD/Dauwpunt) en status-evaluatie.
//  */

// #include "Config.h"
// #include "ClimateLogic.h"
// #include "IndoorSensors.h"
// #include "KasSensors.h"
// #include "Logger.h"
// #include "LedManager.h"

// extern LedManager statusLed;
// extern bool isClimateCrash; 

// bool isSystemReady() {
//   if (currentLuxValue > 1.0) {
//     return (millis() >= 5000); 
//   }
//   return (millis() >= SYSTEM_STARTUP_DELAY);
// }

// void checkClimateCrash(float inTemp) {
//   bool criticalTempHigh = (inTemp >= (GREENHOUSE_CRASH_TEMP + 2.0)); 
//   bool safeTempRecovery = (inTemp <= GREENHOUSE_CRASH_TEMP);       

//   if (!isClimateCrash && criticalTempHigh) {
//     isClimateCrash = true;
//     logToSyslogAndSerialPrintf("[CRASH] KLIMAAT CRASH GEDETECTEERD! Kas temperatuur is kritiek: %.1f°C", inTemp);
//     sendTelegramAlert("🚨 *KRITIEK KLIMAATALARM:* De kas is in een noodtoestand beland (Te heet: " + String(inTemp, 1) + "°C)! Systeem forceert maximale ventilatie.");
//   } 
//   else if (isClimateCrash && safeTempRecovery) {
//     isClimateCrash = false;
//     logToSyslogAndSerialPrintf("[RECOVERY] Klimaat crash voorbij. Kas temperatuur hersteld: %.1f°C", inTemp);
//     sendTelegramAlert("✅ *HERSTEL:* De kas is weer buiten de gevarenzone (Temp: " + String(inTemp, 1) + "°C). Systeem hervat automatische regeling.");
//   }
// }

// void evaluateClimateState(float kasTemp, float kasHum, float indoorTemp, float indoorHum) {
//   if (isnan(kasTemp) || isnan(kasHum)) return;

//   if (!isSystemReady()) {
//     moldRisk = false;
//     moldReasonText = "⏳ Systeem kalibreert / leert...";
//     vpdStatusText = " `(⏳ Opstarten)`";
//     dpMarginStatusText = " `(⏳ Kalibreren)`";
//     return;
//   }

//   kasSmoothedTemp = kasTemp;
//   kasSmoothedHum  = kasHum;
//   displayedTemp   = kasTemp; 

//   kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
//   kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
//   kasDpMargin = kasSmoothedTemp - kasDewPoint;

//   checkClimateCrash(kasSmoothedTemp);

//   bool vpdTooWet   = (kasVpd < VPD_MIN_OPTIMAL);
//   bool vpdTooDry   = (kasVpd > VPD_MAX_OPTIMAL);
//   bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
//   bool humHigh     = (kasSmoothedHum >= HUM_MOLD_THRESHOLD);
//   bool tempTooLow  = (kasSmoothedTemp < HEAT_MAT_TEMP_LOW);
//   bool tempTooHigh = (kasSmoothedTemp > GREENHOUSE_MAX_TEMP);

//   if (isClimateCrash) {
//     moldRisk = true;
//     moldReasonText = "🚨 NOODTOESTAND: Kas uit de hand gelopen (" + String(kasSmoothedTemp, 1) + "°C)!";
//   }
//   else if (vpdTooWet) {
//     moldRisk = true;
//     moldReasonText = "⚠️ SCHIMMELRISICO: Kas te klam (VPD " + String(kasVpd, 2) + " < " + String(VPD_MIN_OPTIMAL, 2) + " kPa)";
//   }
//   else if (vpdTooDry) {
//     moldRisk = false; 
//     String cause = lampsAreOn ? "door lampwarmte" : "door omgevingswarmte/droogte";
//     moldReasonText = "⚠️ UITDROGINGSRISICO: Kas te droog " + cause + " (VPD " + String(kasVpd, 2) + " kPa)";
//   }
//   else if (condensRisk) {
//     moldRisk = true;
//     moldReasonText = "⚠️ KRITIEK: Condensgevaar op zaadjes! Marge: " + String(kasDpMargin, 1) + "°C";
//   } 
//   else if (humHigh) {
//     moldRisk = true;
//     moldReasonText = "⚠️ RISICO: Luchtvochtigheid te hoog (" + String(kasSmoothedHum, 1) + "%)";
//   } 
//   else if (tempTooLow) {
//     moldRisk = false;
//     moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te laag (" + String(kasSmoothedTemp, 1) + "°C)";
//   }
//   else if (tempTooHigh) {
//     moldRisk = false;
//     String cause = lampsAreOn ? "door verlichting" : "door externe warmte";
//     moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te hoog " + cause + " (" + String(kasSmoothedTemp, 1) + "°C)";
//   }
//   else {
//     moldRisk = false;
//     moldReasonText = "✅ Ideaal kiemklimaat";
//   }

//   // LED indicatie voor de kas
//   if (isClimateCrash || tempTooHigh) {
//     statusLed.setColor(255, 0, 0); 
//   } 
//   else if (vpdTooDry || vpdTooWet || condensRisk) {
//     statusLed.setColor(255, 100, 0); 
//   }
//   else {
//     statusLed.setColor(0, 0, 0); 
//   }

//   if (isClimateCrash) vpdStatusText = " `(🚨 NOODSTOP)`";
//   else if (vpdTooWet) vpdStatusText = " `(⚠️ Te klam)`";
//   else if (vpdTooDry) vpdStatusText = " `(🔥 Te droog)`";
//   else vpdStatusText = " `(✅ Optimaal)`";

//   if (condensRisk) dpMarginStatusText = " `(⚠️ Risico)`";
//   else dpMarginStatusText = " `(✅ Veilig)`";

//   if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum)) {
//     indoorDewPoint = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
//     indoorVpd = calcVPD(indoorSmoothedTemp, indoorSmoothedHum);
//     indoorDpMargin = indoorSmoothedTemp - indoorDewPoint;
//   }

//   if (!isnan(outdoorTemp) && !isnan(outdoorHumidity)) {
//     if (outdoorHumidity <= 0.0 || outdoorHumidity > 100.0 || outdoorTemp <= -900.0) {
//       outdoorDewPoint = -999.0;
//       outdoorVpd = -999.0;
//       outdoorDpMargin = -999.0;
//     } else {
//       outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
//       outdoorVpd = calcVPD(outdoorTemp, outdoorHumidity);
//       outdoorDpMargin = outdoorTemp - outdoorDewPoint; 
//     }
//   }
// }

// void updateHeatingAdvice(float inTemp, float outTemp) {
//   if (isnan(inTemp) || isTestModeActive || isKasSleeping) return; 
//   (void)outTemp;
//   if (inTemp > 22.0) {
//     if (isHeatMatRecommended) logToSerial("[HEATING] Kas temperatuur boven 22°C, warmtemat uitgeschakeld.");
//     isHeatMatRecommended = false;
//     heatMatLowStartTime = 0;
//     heatMatHighStartTime = 0;
//     return;
//   }
//   if (inTemp < HEAT_MAT_TEMP_LOW) {
//     heatMatHighStartTime = 0; 
//     if (!isHeatMatRecommended) { 
//       if (heatMatLowStartTime == 0) heatMatLowStartTime = millis(); 
//       else if (millis() - heatMatLowStartTime >= HEAT_MAT_DELAY) {
//         logToSyslogAndSerialPrintf("[HEATING] Kas temperatuur te laag (%.1f°C), warmtemat AANbevolen.", inTemp);
//         isHeatMatRecommended = true; 
//         heatMatLowStartTime = 0;     
//       }
//     }
//   } else if (inTemp > HEAT_MAT_TEMP_HIGH) {
//     heatMatLowStartTime = 0; 
//     if (isHeatMatRecommended) { 
//       if (heatMatHighStartTime == 0) heatMatHighStartTime = millis(); 
//       else if (millis() - heatMatHighStartTime >= HEAT_MAT_DELAY) {
//         logToSyslogAndSerialPrintf("[HEATING] Kas temperatuur OK (%.1f°C), warmtemat UITgeschakeld.", inTemp);
//         isHeatMatRecommended = false; 
//         heatMatHighStartTime = 0;     
//       }
//     }
//   } else {
//     heatMatLowStartTime = 0;
//     heatMatHighStartTime = 0;
//   }
// }

// void checkClimateVelocity(float currentHum) {
//   if (millis() - lastVelocityCheckTime >= CLIMATE_CHECK_INTERVAL_MS || lastVelocityCheckTime == 0) {
//     if (previousHumForVelocity != -999.0) {
//       humidityVelocity = currentHum - previousHumForVelocity; 
//       if (humidityVelocity >= 3.0 && currentHum > 60.0) {
//         logToSyslogAndSerialPrintf("[ALERT] Snelle vochtigheidstoename in kas: +%.1f%%/min", humidityVelocity);
//         kasAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
//       }
//     }
//     previousHumForVelocity = currentHum;
//     lastVelocityCheckTime = millis();
//   }
// }

// void updateMoldRiskHistory(bool currentRisk) {
//   if (millis() - lastMoldSampleTime >= CLIMATE_CHECK_INTERVAL_MS || lastMoldSampleTime == 0) {
//     lastMoldSampleTime = millis();
//     moldHistory[moldSampleIndex] = currentRisk;
//     moldSampleIndex = (moldSampleIndex + 1) % MOLD_SAMPLES;
//     if (moldSampleIndex == 0) moldHistoryFilled = true;

//     int positiveCount = 0;
//     int total = moldHistoryFilled ? MOLD_SAMPLES : moldSampleIndex;
//     for (int i = 0; i < total; i++) { if (moldHistory[i]) positiveCount++; }

//     bool filteredMoldRisk = (positiveCount > (total / 2));
//     if (filteredMoldRisk != previousMoldRisk) {
//       previousMoldRisk = filteredMoldRisk;
//       logToSyslogAndSerial(filteredMoldRisk ? "[MOLD ALERT] Schimmelrisico gedetecteerd!" : "[MOLD RECOVERY] Schimmelrisico geweken.");
//     }
//   }
// }

// void checkKasTrends(float currentTemp, float currentHum, float currentBaro) {
//   if (pastBaro < 0) { pastBaro = currentBaro; lastTrendSample = millis(); return; }
//   (void)currentTemp; (void)currentHum;
//   if (millis() - lastTrendSample > CLIMATE_CHECK_INTERVAL_MS) {
//     float baroDiff = currentBaro - pastBaro;
//     if (baroDiff >= 0.2) baroTrendArrow = "^";
//     else if (baroDiff <= -0.2) baroTrendArrow = "v";
//     else baroTrendArrow = "=";
//     pastBaro = currentBaro;
//     lastTrendSample = millis();
//   }
// }

// void updateHighLow(float currentTemp) { 
//   if (isnan(currentTemp)) return; 
//   (void)currentTemp; 
// }

// void updateHumidityHighLow(float kasHum, float indoorHum, float outHum) { 
//   (void)kasHum; (void)indoorHum; (void)outHum; 
// }