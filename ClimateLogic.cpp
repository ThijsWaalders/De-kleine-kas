/**
 * @file ClimateLogic.cpp
 * @brief Implementatie van robuuste klimaatberekeningen voor kiemgroenten (ESP32-S3).
 *        Maakt strikt gebruik van de Single Source of Truth (Config.h).
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"

// =========================================================================
// 1. VASTE CORRECTIES & KALIBRATIE
// =========================================================================
float kasTempOffset = 0.0; 
float kasHumOffset = 0.0;  

// =========================================================================
// 2. BUFFER / VOORTSCHRIJDEND GEMIDDELDE (Filter voor goedkope sensoren)
// =========================================================================
#define SENSOR_BUFFER_SIZE 5 

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
KasVentState lastAppliedKasAdvice = OFF;

// =========================================================================
// 5. HULPFUNCTIES: BUFFERING & GEMIDDELDES
// =========================================================================
void updateSensorBuffer(float rawTemp, float rawHum, float &avgTemp, float &avgHum, float &avgVpd, float &avgDp) {
  unsigned long currentMillis = millis();

  if (currentMillis - lastBufferSampleTime >= 60000 || lastBufferSampleTime == 0) {
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
// 6. HOOFD KLIMAAT- & SCHIMMELDIAGNOSE (Gekoppeld aan Config.h)
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

  // Gebruik de harde grenzen uit Config.h
  bool vpdTooWet = (kasVpd < VPD_MIN_OPTIMAL);
  bool vpdTooDry = (kasVpd > VPD_MAX_OPTIMAL);
  bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
  bool humHigh = (avgHum >= HUM_MOLD_THRESHOLD);
  bool tempTooLow = (avgTemp < HEAT_MAT_TEMP_LOW);
  bool tempTooHigh = (avgTemp > GREENHOUSE_MAX_TEMP);

  // Evaluatie risico's specifiek voor kiemgroenten
  if (vpdTooWet) {
    moldRisk = true;
    moldReasonText = "⚠️ SCHIMMELRISICO: Kas te klam (VPD " + String(kasVpd, 2) + " < " + String(VPD_MIN_OPTIMAL, 2) + " kPa)";
  }
  else if (vpdTooDry) {
    moldRisk = false; // <--- OPGELOST: Uitdroging is geen schimmelrisico!
    moldReasonText = "⚠️ UITDROGINGSRISICO: Kas te droog (VPD " + String(kasVpd, 2) + " > " + String(VPD_MAX_OPTIMAL, 2) + " kPa)";
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
    moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te hoog (" + String(avgTemp, 1) + "°C)";
  }
  else {
    moldRisk = false;
    moldReasonText = "✅ Ideaal kiemklimaat";
  }

  // UI / Display status iconen
  if (vpdTooWet) vpdStatusText = " `(⚠️ Te klam)`";
  else if (vpdTooDry) vpdStatusText = " `(🔥 Te droog)`";
  else vpdStatusText = " `(✅ Optimaal)`";

  if (condensRisk) dpMarginStatusText = " `(⚠️ Risico)`";
  else dpMarginStatusText = " `(✅ Veilig)`";

  // --- BEREKENINGEN VOOR BINNEN (WONING) ---
  if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum)) {
    float correctedIndoorTemp = indoorSmoothedTemp + INDOOR_TEMP_OFFSET;
    float correctedIndoorHum = indoorSmoothedHum + INDOOR_HUM_OFFSET;
    
    indoorDewPoint = calcDewPoint(correctedIndoorTemp, correctedIndoorHum);
    indoorVpd = calcVPD(correctedIndoorTemp, correctedIndoorHum);
    indoorDpMargin = correctedIndoorTemp - indoorDewPoint;
  }

  // --- BEREKENINGEN VOOR BUITEN ---
  if (!isnan(outdoorTemp) && !isnan(outdoorHumidity)) {
    outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
    outdoorVpd = calcVPD(outdoorTemp, outdoorHumidity);
    outdoorDpMargin = outdoorTemp - outdoorDewPoint; // Essentieel voor je Grafana symmetrie!
  }

}

// =========================================================================
// 7. PROACTIEVE SNELHEIDSCONTROLE (Velocity)
// =========================================================================
void checkClimateVelocity(float currentHum) {
  if (millis() < 180000) return;

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
// void checkClimateVelocity(float currentHum) {
//   if (millis() < 180000) return;

//   if (millis() - lastVelocityCheckTime >= 60000 || lastVelocityCheckTime == 0) {
//     if (previousHumForVelocity != -999.0) {
//       humidityVelocity = currentHum - previousHumForVelocity; 
      
//       if (humidityVelocity >= 3.0 && currentHum > 60.0) {
//         logToSyslogAndSerialPrintf("[ALERT] Snelle vochtigheidstoename in kas: +%.1f%%/min", humidityVelocity);
//         sendTelegramAlert("⚠️ *PROACTIEF ALERT:* Kasvochtigheid stijgt explosief (+" + String(humidityVelocity, 1) + "%/min)! Circulatie preventief gestart.");
//         kasAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
//       }
//     }
//     previousHumForVelocity = currentHum;
//     lastVelocityCheckTime = millis();
//   }
// }

void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;

  if (millis() < 180000) {
    kasAdvice = OFF;
    kasAdviceReason = "Systeem is aan het opstarten...";
    return;
  }

  KasVentState rawNewAdvice = OFF;

  bool isTooWarm = (inTemp > GREENHOUSE_MAX_TEMP);
  bool isKritischHeet = (inTemp > 28.0); // Absolute noodgrens tegen oververhitting/brandgevaar
  bool isTooCold = (inTemp < HEAT_MAT_TEMP_LOW);
  bool isTooWet  = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
  bool isTooDry  = (kasVpd > VPD_MAX_OPTIMAL);
  bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);

  // Vergelijkingen met buiten en binnen
  bool outdoorIsCooler = (outTemp < (inTemp - 0.5)); // Minstens 0.5°C koeler om zin te hebben
  bool indoorIsDrier   = (indoorSmoothedTemp > inTemp); // Eenvoudige buffercheck

  // --- HIERARCHISCHE REGELBESLISTHEID VOOR DE ESP ---

  // 1. KRITISCH HEET / BRANDGEVAAR / OVERVERHITTING
  if (isKritischHeet) {
    rawNewAdvice = GREENHOUSE_VENTILATE;
    kasAdviceReason = "🚨 KRITIEK: Kas is gevaarlijk heet (" + String(inTemp, 1) + "°C)! Direct geforceerd koelen/ventileren!";
  }
  // 2. TE DROOG (Door lampenwarmte) -> KAS DICHT HOUDEN! (Uitdroging voorkomen)
  else if (isTooDry) {
    rawNewAdvice = OFF;
    kasAdviceReason = "🔥 Uitdrogingsrisico door lampen/warmte (VPD " + String(kasVpd, 2) + " kPa); kas blijft gesloten om vocht vast te houden.";
  }
  // 3. TE KLAM / SCHIMMEL- OF CONDENSRISICO
  else if (isTooWet || condensRisk) {
    if (outdoorIsCooler && outHum < 85.0) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = "Kas te klam/condensrisico; advies: buitenlucht inzetten voor droge vochtafvoer.";
    } else {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "Kas te klam; advies: interne circulatie gebruiken om microklimaat te breken.";
    }
  }
  // 4. TE WARM (Normale overschrijding door lampen, maar VPD is nog oké)
  else if (isTooWarm) {
    if (outdoorIsCooler) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = "Kas warm door lampen (" + String(inTemp, 1) + "°C); koelen met koelere buitenlucht (" + String(outTemp, 1) + "°C).";
    } else {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "Kas warm door lampen, maar buiten is het ook warm; interne circulatie om hitteval te breken.";
    }
  }
  // 5. TE KOUD -> Warmte uit woning benutten
  else if (isTooCold && indoorIsDrier) {
    rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "Kas te koud (" + String(inTemp, 1) + "°C); advies: intern circuleren voor warmte uit de woning.";
  }
  // 6. ALLES IN BALANS
  else {
    rawNewAdvice = OFF;
    kasAdviceReason = "Klimaat in de kas is optimaal in balans; lampen en kas draaien stabiel.";
  }

  // Anti-hunt filter toepassen
  kasAdvice = applyAntiHuntFilter(rawNewAdvice);

  // Telegram alerts sturen bij veranderingen van advies voor jou als actuator
  if (kasAdvice != previousKasAdvice) {
    previousKasAdvice = kasAdvice;
    if (kasAdvice == GREENHOUSE_VENTILATE) {
      logToSyslogAndSerialPrintf("[ADVICE] Ventileren geadviseerd. Reden: %s", kasAdviceReason.c_str());
      sendTelegramAlert("🪟 *ACTUATOR ADVIES: Buiten ventileren*\n" + kasAdviceReason);
    } else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
      logToSyslogAndSerialPrintf("[ADVICE] Circulatie geadviseerd. Reden: %s", kasAdviceReason.c_str());
      sendTelegramAlert("🌀 *ACTUATOR ADVIES: Intern circuleren*\n" + kasAdviceReason);
    } else if (kasAdvice == OFF) {
      logToSyslogAndSerial("[ADVICE] Systeem in rust (balans).");
      sendTelegramAlert("✅ *ACTUATOR ADVIES: Kas gesloten / In balans*\nIdeaal kiemklimaat behouden.");
    }
  }
}
// // =========================================================================
// // 8. VENTILATIE- EN VERWARMINGSLOGICA (Voor menselijke actuator / Telegraf / Grafana)
// // =========================================================================
// void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
//   if (isnan(inTemp) || isnan(inHum)) return;

//   if (millis() < 180000) {
//     kasAdvice = OFF;
//     kasAdviceReason = "Systeem is aan het opstarten...";
//     return;
//   }

//   KasVentState rawNewAdvice = OFF;

//   bool isTooWarm = (inTemp > GREENHOUSE_MAX_TEMP);
//   bool isTooCold = (inTemp < HEAT_MAT_TEMP_LOW);
//   bool isTooWet  = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
//   bool isTooDry  = (kasVpd > VPD_MAX_OPTIMAL);
//   bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);

//   // Vergelijkingen met buiten en binnen
//   bool outdoorIsCooler = (outTemp < (inTemp - 0.5)); // Minstens 0.5°C koeler om zin te hebben
//   bool indoorIsDrier   = (indoorSmoothedTemp > inTemp); // Eenvoudige buffercheck

//   // --- LOGICA VOOR MENSELIJKE ACTUATOR ---

//   // 1. REGEL 1: TE WARM? -> Advies om te koelen met buitenlucht (als het buiten ook echt koeler is!)
//   if (isTooWarm) {
//     if (outdoorIsCooler) {
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "Kas te warm (" + String(inTemp, 1) + "°C > max " + String(GREENHOUSE_MAX_TEMP, 1) + "°C); advies: open buitenventilatie (buiten is koeler: " + String(outTemp, 1) + "°C).";
//     } else {
//       rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//       kasAdviceReason = "Kas te warm, maar buiten is het te warm (" + String(outTemp, 1) + "°C); buiten ventileren heeft geen zin, intern circuleren.";
//     }
//   }
//   // 2. REGEL 2: TE KLAM / SCHIMMEL- OF CONDENSRISICO
//   else if (isTooWet || condensRisk) {
//     if (outdoorIsCooler && outHum < 85.0) {
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "Kas te klam/condensrisico; advies: buitenlucht inzetten voor droge vochtafvoer.";
//     } else {
//       rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//       kasAdviceReason = "Kas te klam; advies: interne circulatie gebruiken om microklimaat te breken.";
//     }
//   }
//   // 3. REGEL 3: TE DROOG (Hoge VPD) -> Absoluut gesloten houden om uitdroging kiemen te voorkomen
//   else if (isTooDry) {
//     rawNewAdvice = OFF;
//     kasAdviceReason = "Kas te droog (VPD " + String(kasVpd, 2) + " > max " + String(VPD_MAX_OPTIMAL, 2) + "); advies: kas gesloten houden.";
//   }
//   // 4. REGEL 4: TE KOUD -> Warmte uit woning benutten
//   else if (isTooCold && indoorIsDrier) {
//     rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//     kasAdviceReason = "Kas te koud (" + String(inTemp, 1) + "°C); advies: intern circuleren voor warmte uit de woning.";
//   }
//   // 5. REGEL 5: ALLES IN BALANS
//   else {
//     rawNewAdvice = OFF;
//     kasAdviceReason = "Klimaat in de kas is optimaal in balans; geen actie vereist.";
//   }

//   // Anti-hunt filter toepassen zodat je niet platgebombardeerd wordt met wisselende adviezen
//   kasAdvice = applyAntiHuntFilter(rawNewAdvice);

//   // Telegram alerts sturen bij veranderingen van advies voor jou als actuator
//   if (kasAdvice != previousKasAdvice) {
//     previousKasAdvice = kasAdvice;
//     if (kasAdvice == GREENHOUSE_VENTILATE) {
//       logToSyslogAndSerialPrintf("[ADVICE] Ventileren geadviseerd. Reden: %s", kasAdviceReason.c_str());
//       sendTelegramAlert("🪟 *ACTUATOR ADVIES: Buiten ventileren*\n" + kasAdviceReason);
//     } else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
//       logToSyslogAndSerialPrintf("[ADVICE] Circulatie geadviseerd. Reden: %s", kasAdviceReason.c_str());
//       sendTelegramAlert("🌀 *ACTUATOR ADVIES: Intern circuleren*\n" + kasAdviceReason);
//     } else if (kasAdvice == OFF) {
//       logToSyslogAndSerial("[ADVICE] Systeem in rust (balans).");
//       sendTelegramAlert("✅ *ACTUATOR ADVIES: Kas gesloten / In balans*\nIdeaal kiemklimaat behouden.");
//     }
//   }
// }


void updateHeatingAdvice(float inTemp, float outTemp) {
  if (isnan(inTemp) || isTestModeActive) return; 

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
// 9. HISTORIE & STATISTIEKEN (Onveranderd stabiel)
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
      // Losse sendTelegramAlert hier weglaten, want de evaluatie / ventilatielogica pakt dit al op
    }
  }
}
// void updateMoldRiskHistory(bool currentRisk) {
//   if (millis() - lastMoldSampleTime >= 60000 || lastMoldSampleTime == 0) {
//     lastMoldSampleTime = millis();
//     moldHistory[moldSampleIndex] = currentRisk;
//     moldSampleIndex = (moldSampleIndex + 1) % MOLD_SAMPLES;
//     if (moldSampleIndex == 0) moldHistoryFilled = true;

//     int positiveCount = 0;
//     int total = moldHistoryFilled ? MOLD_SAMPLES : moldSampleIndex;
//     for (int i = 0; i < total; i++) {
//       if (moldHistory[i]) positiveCount++;
//     }

//     bool filteredMoldRisk = (positiveCount > (total / 2));
//     if (filteredMoldRisk != previousMoldRisk) {
//       previousMoldRisk = filteredMoldRisk;
//       logToSyslogAndSerial(filteredMoldRisk ? "[MOLD ALERT] Schimmelrisico gedetecteerd!" : "[MOLD RECOVERY] Schimmelrisico geweken.");
//       sendTelegramAlert(filteredMoldRisk ? "⚠️ *WAARSCHUWING:* Schimmelrisico in de kas!" : "✅ *HERSTEL:* Schimmelrisico geweken.");
//     }
//   }
// }

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
  
  // Als het de eerste keer is (of na 24u), initialiseer direct op de huidige waarde
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