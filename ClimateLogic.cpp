/**
 * @file ClimateLogic.cpp
 * @brief Zuivere klimaatlogica, fysische formules (VPD/Dauwpunt), Klimaatgeheugen & Lamp-compensatie voor ESP32-S3.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"
#include "LedManager.h"

extern LedManager statusLed;

// Externe variabelen uit Config.cpp die hier worden gebruikt
extern bool isClimateCrash;
extern String houseAdviceReason;

// --- FILE-SCOPE FEEDFORWARD VARIABELEN (Bereik voor alle functies in dit bestand) ---
static unsigned long lastLightsOnMillis = 0;
static bool hasStoredLastLightsOn = false;
static bool prevLampsAreOn = false;
static bool upcomingLightsOn = false;

// --- ZELFLERENDE STAAT & KLIMAATGEHEUGEN ---
static float learnedThermalInertia = 1.5f; 
static float tempBeforeFanAction = 0.0f;
static int lastFanSpeedCommand = 0;

static int stateToggleCount = 0;
static unsigned long lastToggleTime = 0;
static float dynamicSetpointOffset = 0.0f; 

// Klimaatgeheugen (Bias voor langetermijntrend: droog vs. klam)
static float learnedClimateBias = 0.0f; // -1.0 (structureel klam) tot +1.0 (structureel droog)
static unsigned long lastBiasCheckTime = 0;



// 3 uur van te voren licht anticipatie
static int currentMinutesUntilLightsOn = 0;
// Getter functie
int getMinutesUntilLightsOn() {
  return currentMinutesUntilLightsOn;
}




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

  // =========================================================================
  // DYNAMISCHE FEEDFORWARD LAMP-ANTICIPATIE (De 21-uur regel)
  // =========================================================================
  // 1. Detecteer en onthoud het exacte moment waarop de lampen aansprongen
  if (lampsAreOn && !prevLampsAreOn) {
    lastLightsOnMillis = millis();
    hasStoredLastLightsOn = true;
    logToSyslogAndSerialPrintf("[FEEDFORWARD] Lampen AAN gedetecteerd. Cyclus-start opgeslagen voor 24-uurs anticipatie.");
  }
  prevLampsAreOn = lampsAreOn;

  // 2. Bereken dynamisch hoeveel minuten het duurt voordat de lampen weer aanspringen (24u cyclus)
  int minutesUntilLightsOn = testMinutesUntilLightsOn; // Fallback op testwaarde zolang er geen historie is
  
  if (hasStoredLastLightsOn && !lampsAreOn) {
    unsigned long elapsedSinceLastOn = millis() - lastLightsOnMillis;
    unsigned long cycleDurationMs = 86400000UL; // 24 uur in milliseconden
    
    if (elapsedSinceLastOn < cycleDurationMs) {
      unsigned long remainingMs = cycleDurationMs - elapsedSinceLastOn;
      minutesUntilLightsOn = remainingMs / 60000UL; // Omzetten naar minuten
    } else {
      minutesUntilLightsOn = 0; // Verwachte starttijd is bereikt/gepasseerd
    }
  } else if (lampsAreOn) {
    minutesUntilLightsOn = 0; // Lampen branden al
  }

  // Sla op in file-scope variabele zodat Telegram de countdown kan opvragen via getMinutesUntilLightsOn()
  currentMinutesUntilLightsOn = minutesUntilLightsOn;

  // 3. Activeer anticipatie als we binnen het 3-uurs venster (180 min) vóór de lampen vallen
  upcomingLightsOn = (!lampsAreOn && minutesUntilLightsOn <= PREEMPTIVE_LAMP_LOOKAHEAD_MINUTES && minutesUntilLightsOn > 0);
  isPreemptiveLampConditioningActive = upcomingLightsOn;

  float targetIndoorVpdMin = 0.80f;

  if (upcomingLightsOn) {
    // Proactief: dwing de woningbuffer alvast naar een drogere/optimale zone (0.95 kPa)
    // 3 uur voordat de lampen aangaan, zodat de woning maximaal vocht kan bufferen.
    targetIndoorVpdMin = 0.95f; 
  } else if (learnedClimateBias < -0.3f) {
    targetIndoorVpdMin = 0.90f; 
  } else if (learnedClimateBias > 0.3f) {
    targetIndoorVpdMin = 0.75f; 
  }

  // =========================================================================
  // LAMP-AWARE DYNAMISCHE VPD GRENZEN
  // =========================================================================
  float currentMaxOptimalVpd = lampsAreOn ? 1.25f : VPD_MAX_OPTIMAL; 
  float currentMinOptimalVpd = lampsAreOn ? 0.60f : VPD_MIN_OPTIMAL;

  bool vpdTooWet   = (kasVpd < currentMinOptimalVpd);
  bool vpdTooDry   = (kasVpd > currentMaxOptimalVpd);
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
    moldReasonText = "⚠️ SCHIMMELRISICO: Kas te klam (VPD " + String(kasVpd, 2) + " < " + String(currentMinOptimalVpd, 2) + " kPa)";
  }
  else if (vpdTooDry) {
    moldRisk = false; 
    String cause = lampsAreOn ? "door lampwarmte (geaccepteerd)" : "door omgevingswarmte/droogte";
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
    moldReasonText = lampsAreOn ? "✅ Ideaal kiemklimaat (inclusief lamp-compensatie)" : "✅ Ideaal kiemklimaat";
  }

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

//   // =========================================================================
//   // DYNAMISCHE FEEDFORWARD LAMP-ANTICIPATIE (De 21-uur regel)
//   // =========================================================================
//   static unsigned long lastLightsOnMillis = 0;
//   static bool hasStoredLastLightsOn = false;
//   static bool prevLampsAreOn = false;

//   // 1. Detecteer en onthoud het exacte moment waarop de lampen aansprongen
//   if (lampsAreOn && !prevLampsAreOn) {
//     lastLightsOnMillis = millis();
//     hasStoredLastLightsOn = true;
//     logToSyslogAndSerialPrintf("[FEEDFORWARD] Lampen AAN gedetecteerd. Cyclus-start opgeslagen voor 24-uurs anticipatie.");
//   }
//   prevLampsAreOn = lampsAreOn;

//   // 2. Bereken dynamisch hoeveel minuten het duurt voordat de lampen weer aanspringen (24u cyclus)
//   int minutesUntilLightsOn = testMinutesUntilLightsOn; // Fallback op testwaarde zolang er geen historie is
  
//   if (hasStoredLastLightsOn && !lampsAreOn) {
//     unsigned long elapsedSinceLastOn = millis() - lastLightsOnMillis;
//     unsigned long cycleDurationMs = 86400000UL; // 24 uur in milliseconden
    
//     if (elapsedSinceLastOn < cycleDurationMs) {
//       unsigned long remainingMs = cycleDurationMs - elapsedSinceLastOn;
//       minutesUntilLightsOn = remainingMs / 60000UL; // Omzetten naar minuten
//     } else {
//       minutesUntilLightsOn = 0; // Verwachte starttijd is bereikt/gepasseerd
//     }
//   } else if (lampsAreOn) {
//     minutesUntilLightsOn = 0; // Lampen branden al
//   }

//   // 3. Activeer anticipatie als we binnen het 3-uurs venster (180 min) vóór de lampen vallen
//   bool upcomingLightsOn = (!lampsAreOn && minutesUntilLightsOn <= PREEMPTIVE_LAMP_LOOKAHEAD_MINUTES && minutesUntilLightsOn > 0);
//   isPreemptiveLampConditioningActive = upcomingLightsOn;

//   float targetIndoorVpdMin = 0.80f;

//   if (upcomingLightsOn) {
//     // Proactief: dwing de woningbuffer alvast naar een drogere/optimale zone (0.95 kPa)
//     // 3 uur voordat de lampen aangaan, zodat de woning maximaal vocht kan bufferen.
//     targetIndoorVpdMin = 0.95f; 
//   } else if (learnedClimateBias < -0.3f) {
//     targetIndoorVpdMin = 0.90f; 
//   } else if (learnedClimateBias > 0.3f) {
//     targetIndoorVpdMin = 0.75f; 
//   }

//   // =========================================================================
//   // LAMP-AWARE DYNAMISCHE VPD GRENZEN
//   // =========================================================================
//   float currentMaxOptimalVpd = lampsAreOn ? 1.25f : VPD_MAX_OPTIMAL; 
//   float currentMinOptimalVpd = lampsAreOn ? 0.60f : VPD_MIN_OPTIMAL;

//   bool vpdTooWet   = (kasVpd < currentMinOptimalVpd);
//   bool vpdTooDry   = (kasVpd > currentMaxOptimalVpd);
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
//     moldReasonText = "⚠️ SCHIMMELRISICO: Kas te klam (VPD " + String(kasVpd, 2) + " < " + String(currentMinOptimalVpd, 2) + " kPa)";
//   }
//   else if (vpdTooDry) {
//     moldRisk = false; 
//     String cause = lampsAreOn ? "door lampwarmte (geaccepteerd)" : "door omgevingswarmte/droogte";
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
//     moldReasonText = lampsAreOn ? "✅ Ideaal kiemklimaat (inclusief lamp-compensatie)" : "✅ Ideaal kiemklimaat";
//   }

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

void applySelfLearning(float currentTemp, float targetTemp, int &fanSpeed) {
  unsigned long now = millis();

  if (lastFanSpeedCommand > 0 && fanSpeed == 0) {
    float tempDelta = currentTemp - tempBeforeFanAction;
    if (abs(tempDelta) > 0.1f) {
      learnedThermalInertia = (1.0f - 0.05f) * learnedThermalInertia + 0.05f * abs(tempDelta);
      logToSyslogAndSerialPrintf("[AI LEARN] Geleerde thermische traagheid (Inertia): %.2f", learnedThermalInertia);
    }
  }

  if (lastFanSpeedCommand == 0 && fanSpeed > 0) {
    tempBeforeFanAction = currentTemp;
  }
  lastFanSpeedCommand = fanSpeed;

  bool currentTrendUp = (currentTemp > targetTemp + dynamicSetpointOffset);
  static bool previousTrendUp = false;

  if (currentTrendUp != previousTrendUp) {
    if (now - lastToggleTime < 120000UL) { 
      stateToggleCount++;
      if (stateToggleCount >= 6) { 
        dynamicSetpointOffset += (currentTrendUp ? 0.05f : -0.05f);
        stateToggleCount = 0;
        logToSyslogAndSerialPrintf("[AI LEARN] Oscillatie gedetecteerd. Dynamische setpoint offset aangepast naar: %.2f°C", dynamicSetpointOffset);
      }
    } else {
      stateToggleCount = 0; 
    }
    lastToggleTime = now;
    previousTrendUp = currentTrendUp;
  }
}

void applyClimateBiasLearning(float currentVpd) {
  unsigned long now = millis();
  if (now - lastBiasCheckTime >= 300000UL || lastBiasCheckTime == 0) {
    lastBiasCheckTime = now;

    if (currentVpd > VPD_MAX_OPTIMAL) {
      learnedClimateBias += 0.05f;
      if (learnedClimateBias > 1.0f) learnedClimateBias = 1.0f;
    } else if (currentVpd < VPD_MIN_OPTIMAL) {
      learnedClimateBias -= 0.05f;
      if (learnedClimateBias < -1.0f) learnedClimateBias = -1.0f;
    } else {
      if (learnedClimateBias > 0.0f) learnedClimateBias -= 0.02f;
      if (learnedClimateBias < 0.0f) learnedClimateBias += 0.02f;
    }

    if (abs(learnedClimateBias) > 0.3f) {
      logToSyslogAndSerialPrintf("[AI LEARN] Klimaatbias bijgesteld: %.2f (%s)", 
        learnedClimateBias, 
        (learnedClimateBias > 0 ? "Structureel droogtegevoelig" : "Structureel vochtgevoelig"));
    }
  }
}

void calculateAdaptiveClimate() {
  unsigned long now = millis();
  float deltaTime = (now - lastPidTime) / 1000.0f;
  if (deltaTime < 1.0f) return; 
  lastPidTime = now;

  if (isClimateCrash) {
    fanIntSpeed = 255;
    fanExt1Speed = 255;
    ledcWrite(PIN_FAN_INT_PWM, 255);
    ledcWrite(PIN_FAN_EXT1_PWM, 255);
    return;
  }

  applyClimateBiasLearning(kasVpd);

  // =========================================================================
  // Slimmer Woning vs Buiten Advies
  // =========================================================================
  static HouseVentState activeAdviceState = HOUSE_CLOSED;
  static bool pendingEscalationCheck = false;
  static unsigned long escalationTimer = 0;
  static float baselineIndoorHum = -999.0f;
  static float baselineKasVpd = -999.0f;

  if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum) && !isnan(outdoorTemp) && outdoorHumidity > 0.0f) {
    
    float inDp  = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
    float outDp = calcDewPoint(outdoorTemp, outdoorHumidity);

    bool kasIsDumpingMoisture = (fanExt1Speed > 0 || kasSmoothedHum > 75.0f || kasVpd > VPD_MAX_OPTIMAL || kasVpd < VPD_MIN_OPTIMAL);
    bool houseTooHumid = (indoorSmoothedHum > 62.0f);
    
    float targetIndoorVpdMin = 0.80f;

    if (learnedClimateBias < -0.3f) {
      targetIndoorVpdMin = 0.90f; 
    } else if (learnedClimateBias > 0.3f) {
      targetIndoorVpdMin = 0.75f; 
    }

    bool houseVpdTooLow = (!isnan(indoorVpd) && indoorVpd < targetIndoorVpdMin); 

    bool outsideAirIsSignificantlyDrier = (outDp < (inDp - 1.5f));
    bool forecastBadWeather = (outdoorHumidity > 85.0f || forecastedTempSoon < 0.0f);

    HouseVentState desiredAdvice = HOUSE_CLOSED;
    String newReason = "";

    float projectedIndoorHum = indoorSmoothedHum + (humidityVelocity * 15.0f);

    if ((houseTooHumid || houseVpdTooLow || kasIsDumpingMoisture || projectedIndoorHum > 63.0f) && outsideAirIsSignificantlyDrier && !forecastBadWeather) {
      desiredAdvice = HOUSE_VENTILATE;
      if (upcomingLightsOn && houseVpdTooLow) {
        newReason = "🔮 ANTICIPATIE: Lampen gaan binnenkort aan. Optimaliseer woningbuffer alvast (buitenlucht is droger).";
      } else if (houseVpdTooLow) {
        newReason = "Woning-VPD te laag (buffer raakt vol; optimaliseer uitwisseling); buitenlucht is droger.";
      } else if (kasIsDumpingMoisture && houseTooHumid) {
        newReason = "Kas dumpt vocht & woning klam. Buitenlucht is gunstig; ventileer woning.";
      } else {
        newReason = "Woning te klam of stijgende trend; buitenlucht is droger om buffer te herstellen.";
      }
    } else {
      desiredAdvice = HOUSE_CLOSED;
      if (forecastBadWeather) {
        newReason = "Woning & Kas in balans; naderend vocht/weeromslag volgens API-voorspelling.";
      } else if (!outsideAirIsSignificantlyDrier) {
        newReason = "Woning & Kas in balans; buitenlucht biedt te weinig dauwpunt-marge voor woningbuffer).";
      } else {
        newReason = "Woningklimaat stabiel; buffer heeft voldoende capaciteit voor de kas.";
      }
    }

    unsigned long evalNow = millis();
    if (desiredAdvice != activeAdviceState) {
      if (!pendingEscalationCheck) {
        pendingEscalationCheck = true;
        escalationTimer = evalNow;
        baselineIndoorHum = indoorSmoothedHum;
        baselineKasVpd = kasVpd;
        logToSyslogAndSerialPrintf("[HOUSE ADVICE] Klimaatverschuiving of buffer-actie gedetecteerd. Start 15-minuten evaluatievenster.");
      } else if (evalNow - escalationTimer >= 900000UL) { 
        bool worsening = (indoorSmoothedHum > baselineIndoorHum + 1.0f) || (kasVpd > baselineKasVpd + 0.15f);
        bool improving = (indoorSmoothedHum < baselineIndoorHum) || (kasVpd < baselineKasVpd);

        if (worsening) {
          String escMsg = "⚠️ *ESCALATIE KLIMAAT:* Woningbuffer / Kas VPD blijft oplopen na 15 min! Extra maatregelen vereist.";
          logToSyslogAndSerialPrintf("[HOUSE ADVICE ESCALATION] Klimaat blijft escaleren na 15 minuten.");
          sendTelegramAlert(escMsg);
        } else if (improving) {
          String succMsg = "✅ *SUCCESBEVESTIGING:* Woningventilatie effectief; buffer hersteld. Bereid sluiting voor.";
          logToSyslogAndSerialPrintf("[HOUSE ADVICE SUCCESS] Ventileren effectief, klimaat stabiliseert.");
          sendTelegramAlert(succMsg);
        }

        activeAdviceState = desiredAdvice;
        houseAdvice = desiredAdvice;
        houseAdviceReason = newReason;
        pendingEscalationCheck = false;
      }
    } else {
      pendingEscalationCheck = false;
      houseAdvice = desiredAdvice;
      houseAdviceReason = newReason;
    }
  }

  // =========================================================================
  // PID & VENTILATOR STURING
  // =========================================================================
  float lampTemperatureOffset = lampsAreOn ? 2.0f : 0.0f;
  float targetTemp = 22.0f + dynamicSetpointOffset + lampTemperatureOffset; 
  
  float currentTemp = kasSmoothedTemp;
  float error = currentTemp - targetTemp;
  pError = error;

  float Kp = (abs(error) > 2.0f) ? 18.0f : 8.0f; 
  float Ki = 0.3f;
  float Kd = 2.0f;

  iError += error * deltaTime;
  if (iError > 30.0f) iError = 30.0f;
  if (iError < -30.0f) iError = -30.0f;

  dError = (error - lastError) / deltaTime;
  lastError = error;

  float pidOutput = (Kp * pError) + (Ki * iError) + (Kd * dError);

  if (isLampTransitionActive) {
    unsigned long transitionElapsed = now - lampTransitionStartTime;
    if (transitionElapsed < 600000UL) pidOutput += 10.0f; 
    else isLampTransitionActive = false; 
  }

  int rawPidOutput = (int)pidOutput;

  // Interne fan (circulatie)
  int finalIntSpeed = 0;
  if (moldRisk || kasSmoothedHum > HUM_MOLD_THRESHOLD || rawPidOutput > -5.0f) {
    if (fanIntSpeed == 0) finalIntSpeed = INT_FAN_BASE_PWM;
    else finalIntSpeed = max(INT_FAN_MIN_PWM, min(rawPidOutput, 255));
  } else {
    finalIntSpeed = 0;
  }

  // Externe fans (Proportioneel + Asymmetrisch uitrollen)
  int targetExt1Speed = 0;
  if (error > 0.2f || rawPidOutput > 30.0f) {
    float factor = (pidOutput - 30.0f) / 100.0f;
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    
    targetExt1Speed = EXT_FAN_BASE_PWM + (int)(factor * (FAN_MAX_PWM - EXT_FAN_BASE_PWM));
    if (targetExt1Speed < EXT_FAN_MIN_PWM) targetExt1Speed = EXT_FAN_MIN_PWM;
    if (targetExt1Speed > FAN_MAX_PWM) targetExt1Speed = FAN_MAX_PWM;
  } else {
    targetExt1Speed = 0;
  }

  int finalExt1Speed = fanExt1Speed;
  if (targetExt1Speed > finalExt1Speed) {
    finalExt1Speed = targetExt1Speed;
  } else if (targetExt1Speed < finalExt1Speed) {
    finalExt1Speed -= 3; 
    if (finalExt1Speed < EXT_FAN_MIN_PWM && finalExt1Speed > 0) {
      finalExt1Speed = 0; 
    }
    if (targetExt1Speed == 0 && finalExt1Speed < EXT_FAN_MIN_PWM) {
      finalExt1Speed = 0;
    }
  }

  applySelfLearning(currentTemp, targetTemp, finalExt1Speed);

  fanIntSpeed = finalIntSpeed;
  fanExt1Speed = finalExt1Speed;

  ledcWrite(PIN_FAN_INT_PWM, fanIntSpeed);
  ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
} // <-- DEZE SLUITING WAS HIER MENSEN-OF-MACHINE GEWORDEN

void calculateTrayClimate() {
  if (tray1Mode != 3) { 
    float targetMin1 = getTrayMinTemp(tray1Mode);
    float targetMax1 = getTrayMaxTemp(tray1Mode);
    float currentTray1 = traySensor1Connected ? traySensor1Temp : targetMin1;
    (void)targetMin1; (void)targetMax1; (void)currentTray1;
  }
  if (tray2Mode != 3) {
    float targetMin2 = getTrayMinTemp(tray2Mode);
    float targetMax2 = getTrayMaxTemp(tray2Mode);
    float currentTray2 = traySensor2Connected ? traySensor2Temp : targetMin2;
    (void)targetMin2; (void)targetMax2; (void)currentTray2;
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
  if (isnan(currentTemp) || currentTemp <= -999.0f) return;
  if (currentTemp > kasTempHigh) kasTempHigh = currentTemp;
  if (currentTemp < kasTempLow) kasTempLow = currentTemp;
}

void updateHumidityHighLow(float kasHum, float indoorHum, float outHum) { 
  (void)kasHum; (void)indoorHum; (void)outHum; 
}


// /**
//  * @file ClimateLogic.cpp
//  * @brief Zuivere klimaatlogica, fysische formules (VPD/Dauwpunt) en 3-zone regeling met Klimaatgeheugen voor ESP32-S3.
//  */

// #include "Config.h"
// #include "ClimateLogic.h"
// #include "IndoorSensors.h"
// #include "KasSensors.h"
// #include "Logger.h"
// #include "LedManager.h"

// extern LedManager statusLed;

// // Externe variabelen uit Config.cpp die hier worden gebruikt
// extern bool isClimateCrash;
// extern String houseAdviceReason;

// // --- ZELFLERENDE STAAT & KLIMAATGEHEUGEN ---
// static float learnedThermalInertia = 1.5f; 
// static float tempBeforeFanAction = 0.0f;
// static int lastFanSpeedCommand = 0;

// static int stateToggleCount = 0;
// static unsigned long lastToggleTime = 0;
// static float dynamicSetpointOffset = 0.0f; 

// // Klimaatgeheugen (Bias voor langetermijntrend: droog vs. klam)
// static float learnedClimateBias = 0.0f; // -1.0 (structureel klam) tot +1.0 (structureel droog)
// static unsigned long lastBiasCheckTime = 0;

// bool isSystemReady() {
//   if (currentLuxValue > SYSTEM_READY_LUX_THRESHOLD) {
//     return (millis() >= SYSTEM_READY_SHORT_DELAY_MS); 
//   }
//   return (millis() >= SYSTEM_STARTUP_DELAY);
// }

// void checkClimateCrash(float inTemp) {
//   bool criticalTempHigh = (inTemp >= (GREENHOUSE_CRASH_TEMP + GREENHOUSE_CRASH_OFFSET)); 
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

// void applySelfLearning(float currentTemp, float targetTemp, int &fanSpeed) {
//   unsigned long now = millis();

//   if (lastFanSpeedCommand > 0 && fanSpeed == 0) {
//     float tempDelta = currentTemp - tempBeforeFanAction;
//     if (abs(tempDelta) > 0.1f) {
//       learnedThermalInertia = (1.0f - 0.05f) * learnedThermalInertia + 0.05f * abs(tempDelta);
//       logToSyslogAndSerialPrintf("[AI LEARN] Geleerde thermische traagheid (Inertia): %.2f", learnedThermalInertia);
//     }
//   }

//   if (lastFanSpeedCommand == 0 && fanSpeed > 0) {
//     tempBeforeFanAction = currentTemp;
//   }
//   lastFanSpeedCommand = fanSpeed;

//   bool currentTrendUp = (currentTemp > targetTemp + dynamicSetpointOffset);
//   static bool previousTrendUp = false;

//   if (currentTrendUp != previousTrendUp) {
//     if (now - lastToggleTime < 120000UL) { 
//       stateToggleCount++;
//       if (stateToggleCount >= 6) { 
//         dynamicSetpointOffset += (currentTrendUp ? 0.05f : -0.05f);
//         stateToggleCount = 0;
//         logToSyslogAndSerialPrintf("[AI LEARN] Oscillatie gedetecteerd. Dynamische setpoint offset aangepast naar: %.2f°C", dynamicSetpointOffset);
//       }
//     } else {
//       stateToggleCount = 0; 
//     }
//     lastToggleTime = now;
//     previousTrendUp = currentTrendUp;
//   }
// }

// // Klimaatgeheugen: leert of de kas structureel te droog of te klam neigt te staan
// void applyClimateBiasLearning(float currentVpd) {
//   unsigned long now = millis();
//   if (now - lastBiasCheckTime >= 300000UL || lastBiasCheckTime == 0) {
//     lastBiasCheckTime = now;

//     if (currentVpd > VPD_MAX_OPTIMAL) {
//       learnedClimateBias += 0.05f;
//       if (learnedClimateBias > 1.0f) learnedClimateBias = 1.0f;
//     } else if (currentVpd < VPD_MIN_OPTIMAL) {
//       learnedClimateBias -= 0.05f;
//       if (learnedClimateBias < -1.0f) learnedClimateBias = -1.0f;
//     } else {
//       if (learnedClimateBias > 0.0f) learnedClimateBias -= 0.02f;
//       if (learnedClimateBias < -1.0f + 1.0f && learnedClimateBias < 0.0f) learnedClimateBias += 0.02f;
//     }

//     if (abs(learnedClimateBias) > 0.3f) {
//       logToSyslogAndSerialPrintf("[AI LEARN] Klimaatbias bijgesteld: %.2f (%s)", 
//         learnedClimateBias, 
//         (learnedClimateBias > 0 ? "Structureel droogtegevoelig" : "Structureel vochtgevoelig"));
//     }
//   }
// }

// void calculateAdaptiveClimate() {
//   unsigned long now = millis();
//   float deltaTime = (now - lastPidTime) / 1000.0f;
//   if (deltaTime < 1.0f) return; 
//   lastPidTime = now;

//   if (isClimateCrash) {
//     fanIntSpeed = 255;
//     fanExt1Speed = 255;
//     ledcWrite(PIN_FAN_INT_PWM, 255);
//     ledcWrite(PIN_FAN_EXT1_PWM, 255);
//     return;
//   }

//   // Update het klimaatgeheugen op basis van de huidige kas-VPD
//   applyClimateBiasLearning(kasVpd);

//   // =========================================================================
//   // Slimmer Woning vs Buiten Advies (Inclusief Woning-VPD Target & Klimaatgeheugen)
//   // =========================================================================
//   static HouseVentState activeAdviceState = HOUSE_CLOSED;
//   static bool pendingEscalationCheck = false;
//   static unsigned long escalationTimer = 0;
//   static float baselineIndoorHum = -999.0f;
//   static float baselineKasVpd = -999.0f;

//   if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum) && !isnan(outdoorTemp) && outdoorHumidity > 0.0f) {
    
//     // Fysische Dauwpuntvergelijking (Kas -> Woning -> Buiten)
//     float inDp  = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
//     float outDp = calcDewPoint(outdoorTemp, outdoorHumidity);

//     bool kasIsDumpingMoisture = (fanExt1Speed > 0 || kasSmoothedHum > 75.0f || kasVpd > VPD_MAX_OPTIMAL || kasVpd < VPD_MIN_OPTIMAL);
//     bool houseTooHumid = (indoorSmoothedHum > 62.0f);
    
//     // --- WONING-VPD ALS ZELFSTANDIGE STUURWAARDE & BUFFER BEWAKING ---
//     // Ideale woning VPD zone: 0.80 tot 1.10 kPa. 
//     // We passen de drempel aan op basis van het geleerde klimaatgeheugen (learnedClimateBias):
//     float targetIndoorVpdMin = 0.80f;
//     float targetIndoorVpdMax = 1.10f;

//     if (learnedClimateBias < -0.3f) {
//       targetIndoorVpdMin = 0.90f; // Kas is klam -> dwing woningbuffer strenger/droger af
//     } else if (learnedClimateBias > 0.3f) {
//       targetIndoorVpdMin = 0.75f; // Kas is droog -> woningbuffer hoeft minder agressief droog
//     }

//     bool houseVpdTooLow = (!isnan(indoorVpd) && indoorVpd < targetIndoorVpdMin); // Buffer dreigt te klam te worden

//     // Buitenlucht moet significant droger zijn (minstens 1.5°C in dauwpunt)
//     bool outsideAirIsSignificantlyDrier = (outDp < (inDp - 1.5f));
//     bool forecastBadWeather = (outdoorHumidity > 85.0f || forecastedTempSoon < 0.0f);

//     HouseVentState desiredAdvice = HOUSE_CLOSED;
//     String newReason = "";

//     float projectedIndoorHum = indoorSmoothedHum + (humidityVelocity * 15.0f);

//     if ((houseTooHumid || houseVpdTooLow || kasIsDumpingMoisture || projectedIndoorHum > 63.0f) && outsideAirIsSignificantlyDrier && !forecastBadWeather) {
//       desiredAdvice = HOUSE_VENTILATE;
//       if (houseVpdTooLow) {
//         newReason = "Woning-VPD te laag (buffer raakt vol; optimaliseer uitwisseling); buitenlucht is droger.";
//       } else if (kasIsDumpingMoisture && houseTooHumid) {
//         newReason = "Kas dumpt vocht & woning klam. Buitenlucht is gunstig; ventileer woning.";
//       } else {
//         newReason = "Woning te klam of stijgende trend; buitenlucht is droger om buffer te herstellen.";
//       }
//     } else {
//       desiredAdvice = HOUSE_CLOSED;
//       if (forecastBadWeather) {
//         newReason = "Woning/Kas in balans, maar naderend vocht/weeromslag volgens API-voorspelling.";
//       } else if (!outsideAirIsSignificantlyDrier) {
//         newReason = "Woning/Kas in balans (buitenlucht biedt te weinig dauwpunt-marge voor woningbuffer).";
//       } else {
//         newReason = "Woningklimaat stabiel; buffer heeft voldoende capaciteit voor de kas.";
//       }
//     }

//     // Traagheids- en Evaluatiemechanisme (15 Minuten Venster)
//     unsigned long evalNow = millis();
//     if (desiredAdvice != activeAdviceState) {
//       if (!pendingEscalationCheck) {
//         pendingEscalationCheck = true;
//         escalationTimer = evalNow;
//         baselineIndoorHum = indoorSmoothedHum;
//         baselineKasVpd = kasVpd;
//         logToSyslogAndSerialPrintf("[HOUSE ADVICE] Klimaatverschuiving of buffer-actie gedetecteerd. Start 15-minuten evaluatievenster.");
//       } else if (evalNow - escalationTimer >= 900000UL) { // 15 minuten
//         bool worsening = (indoorSmoothedHum > baselineIndoorHum + 1.0f) || (kasVpd > baselineKasVpd + 0.15f);
//         bool improving = (indoorSmoothedHum < baselineIndoorHum) || (kasVpd < baselineKasVpd);

//         if (worsening) {
//           String escMsg = "⚠️ *ESCALATIE KLIMAAT:* Woningbuffer / Kas VPD blijft oplopen na 15 min! Extra maatregelen vereist.";
//           logToSyslogAndSerialPrintf("[HOUSE ADVICE ESCALATION] Klimaat blijft escaleren na 15 minuten.");
//           sendTelegramAlert(escMsg);
//         } else if (improving) {
//           String succMsg = "✅ *SUCCESBEVESTIGING:* Woningventilatie effectief; buffer hersteld. Bereid sluiting voor.";
//           logToSyslogAndSerialPrintf("[HOUSE ADVICE SUCCESS] Ventileren effectief, klimaat stabiliseert.");
//           sendTelegramAlert(succMsg);
//         }

//         activeAdviceState = desiredAdvice;
//         houseAdvice = desiredAdvice;
//         houseAdviceReason = newReason;
//         pendingEscalationCheck = false;
//       }
//     } else {
//       pendingEscalationCheck = false;
//       houseAdvice = desiredAdvice;
//       houseAdviceReason = newReason;
//     }
//   }

//   float targetTemp = 22.0f + dynamicSetpointOffset; 
//   float currentTemp = kasSmoothedTemp;
//   float error = currentTemp - targetTemp;
//   pError = error;

//   float Kp = (abs(error) > 2.0f) ? 18.0f : 8.0f; 
//   float Ki = 0.3f;
//   float Kd = 2.0f;

//   iError += error * deltaTime;
//   if (iError > 30.0f) iError = 30.0f;
//   if (iError < -30.0f) iError = -30.0f;

//   dError = (error - lastError) / deltaTime;
//   lastError = error;

//   float pidOutput = (Kp * pError) + (Ki * iError) + (Kd * dError);

//   if (isLampTransitionActive) {
//     unsigned long transitionElapsed = now - lampTransitionStartTime;
//     if (transitionElapsed < 600000UL) pidOutput += 10.0f; 
//     else isLampTransitionActive = false; 
//   }

//   int rawPidOutput = (int)pidOutput;

//   // Interne fan (circulatie)
//   int finalIntSpeed = 0;
//   if (moldRisk || kasSmoothedHum > HUM_MOLD_THRESHOLD || rawPidOutput > -5.0f) {
//     if (fanIntSpeed == 0) finalIntSpeed = INT_FAN_BASE_PWM;
//     else finalIntSpeed = max(INT_FAN_MIN_PWM, min(rawPidOutput, 255));
//   } else {
//     finalIntSpeed = 0;
//   }

//   // Externe fans (Proportioneel + Asymmetrisch uitrollen)
//   int targetExt1Speed = 0;
//   if (error > 0.2f || rawPidOutput > 30.0f) {
//     float factor = (pidOutput - 30.0f) / 100.0f;
//     if (factor < 0.0f) factor = 0.0f;
//     if (factor > 1.0f) factor = 1.0f;
    
//     targetExt1Speed = EXT_FAN_BASE_PWM + (int)(factor * (FAN_MAX_PWM - EXT_FAN_BASE_PWM));
//     if (targetExt1Speed < EXT_FAN_MIN_PWM) targetExt1Speed = EXT_FAN_MIN_PWM;
//     if (targetExt1Speed > FAN_MAX_PWM) targetExt1Speed = FAN_MAX_PWM;
//   } else {
//     targetExt1Speed = 0;
//   }

//   int finalExt1Speed = fanExt1Speed;
//   if (targetExt1Speed > finalExt1Speed) {
//     finalExt1Speed = targetExt1Speed;
//   } else if (targetExt1Speed < finalExt1Speed) {
//     finalExt1Speed -= 3; 
//     if (finalExt1Speed < EXT_FAN_MIN_PWM && finalExt1Speed > 0) {
//       finalExt1Speed = 0; 
//     }
//     if (targetExt1Speed == 0 && finalExt1Speed < EXT_FAN_MIN_PWM) {
//       finalExt1Speed = 0;
//     }
//   }

//   applySelfLearning(currentTemp, targetTemp, finalExt1Speed);

//   fanIntSpeed = finalIntSpeed;
//   fanExt1Speed = finalExt1Speed;

//   ledcWrite(PIN_FAN_INT_PWM, fanIntSpeed);
//   ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
// }

// void calculateTrayClimate() {
//   if (tray1Mode != 3) { 
//     float targetMin1 = getTrayMinTemp(tray1Mode);
//     float targetMax1 = getTrayMaxTemp(tray1Mode);
//     float currentTray1 = traySensor1Connected ? traySensor1Temp : targetMin1;
//     (void)targetMin1; (void)targetMax1; (void)currentTray1;
//   }
//   if (tray2Mode != 3) {
//     float targetMin2 = getTrayMinTemp(tray2Mode);
//     float targetMax2 = getTrayMaxTemp(tray2Mode);
//     float currentTray2 = traySensor2Connected ? traySensor2Temp : targetMin2;
//     (void)targetMin2; (void)targetMax2; (void)currentTray2;
//   }
// }

// void updateHeatingAdvice(float inTemp, float outTemp) {
//   if (isnan(inTemp) || isTestModeActive || isKasSleeping) return; 
//   (void)outTemp;
//   if (inTemp > HEAT_MAT_DISABLE_TEMP) {
//     if (isHeatMatRecommended) logToSerial("[HEATING] Kas temperatuur boven grenswaarde, warmtemat uitgeschakeld.");
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
//       if (humidityVelocity >= HUMIDITY_VELOCITY_THRESHOLD && currentHum > HUMIDITY_VELOCITY_MIN_HUM) {
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
//     if (baroDiff >= BARO_TREND_THRESHOLD) baroTrendArrow = "^";
//     else if (baroDiff <= -BARO_TREND_THRESHOLD) baroTrendArrow = "v";
//     else baroTrendArrow = "=";
//     pastBaro = currentBaro;
//     lastTrendSample = millis();
//   }
// }

// void updateHighLow(float currentTemp) {
//   if (isnan(currentTemp) || currentTemp <= -999.0f) return;
//   if (currentTemp > kasTempHigh) kasTempHigh = currentTemp;
//   if (currentTemp < kasTempLow) kasTempLow = currentTemp;
// }

// void updateHumidityHighLow(float kasHum, float indoorHum, float outHum) { 
//   (void)kasHum; (void)indoorHum; (void)outHum; 
// }

// // /**
// //  * @file ClimateLogic.cpp
// //  * @brief Zuivere klimaatlogica, fysische formules (VPD/Dauwpunt) en 3-zone regeling voor ESP32-S3.
// //  */

// // #include "Config.h"
// // #include "ClimateLogic.h"
// // #include "IndoorSensors.h"
// // #include "KasSensors.h"
// // #include "Logger.h"
// // #include "LedManager.h"

// // extern LedManager statusLed;

// // // Externe variabelen uit Config.cpp die hier worden gebruikt
// // extern bool isClimateCrash;
// // extern String houseAdviceReason;

// // // --- ZELFLERENDE STAAT VARIABELEN ---
// // static float learnedThermalInertia = 1.5f; 
// // static float tempBeforeFanAction = 0.0f;
// // static int lastFanSpeedCommand = 0;

// // static int stateToggleCount = 0;
// // static unsigned long lastToggleTime = 0;
// // static float dynamicSetpointOffset = 0.0f; 

// // bool isSystemReady() {
// //   if (currentLuxValue > SYSTEM_READY_LUX_THRESHOLD) {
// //     return (millis() >= SYSTEM_READY_SHORT_DELAY_MS); 
// //   }
// //   return (millis() >= SYSTEM_STARTUP_DELAY);
// // }

// // void checkClimateCrash(float inTemp) {
// //   bool criticalTempHigh = (inTemp >= (GREENHOUSE_CRASH_TEMP + GREENHOUSE_CRASH_OFFSET)); 
// //   bool safeTempRecovery = (inTemp <= GREENHOUSE_CRASH_TEMP);       

// //   if (!isClimateCrash && criticalTempHigh) {
// //     isClimateCrash = true;
// //     logToSyslogAndSerialPrintf("[CRASH] KLIMAAT CRASH GEDETECTEERD! Kas temperatuur is kritiek: %.1f°C", inTemp);
// //     sendTelegramAlert("🚨 *KRITIEK KLIMAATALARM:* De kas is in een noodtoestand beland (Te heet: " + String(inTemp, 1) + "°C)! Systeem forceert maximale ventilatie.");
// //   } 
// //   else if (isClimateCrash && safeTempRecovery) {
// //     isClimateCrash = false;
// //     logToSyslogAndSerialPrintf("[RECOVERY] Klimaat crash voorbij. Kas temperatuur hersteld: %.1f°C", inTemp);
// //     sendTelegramAlert("✅ *HERSTEL:* De kas is weer buiten de gevarenzone (Temp: " + String(inTemp, 1) + "°C). Systeem hervat automatische regeling.");
// //   }
// // }

// // void evaluateClimateState(float kasTemp, float kasHum, float indoorTemp, float indoorHum) {
// //   if (isnan(kasTemp) || isnan(kasHum)) return;

// //   if (!isSystemReady()) {
// //     moldRisk = false;
// //     moldReasonText = "⏳ Systeem kalibreert / leert...";
// //     vpdStatusText = " `(⏳ Opstarten)`";
// //     dpMarginStatusText = " `(⏳ Kalibreren)`";
// //     return;
// //   }

// //   kasSmoothedTemp = kasTemp;
// //   kasSmoothedHum  = kasHum;
// //   displayedTemp   = kasTemp; 

// //   kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
// //   kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
// //   kasDpMargin = kasSmoothedTemp - kasDewPoint;

// //   checkClimateCrash(kasSmoothedTemp);

// //   bool vpdTooWet   = (kasVpd < VPD_MIN_OPTIMAL);
// //   bool vpdTooDry   = (kasVpd > VPD_MAX_OPTIMAL);
// //   bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
// //   bool humHigh     = (kasSmoothedHum >= HUM_MOLD_THRESHOLD);
// //   bool tempTooLow  = (kasSmoothedTemp < HEAT_MAT_TEMP_LOW);
// //   bool tempTooHigh = (kasSmoothedTemp > GREENHOUSE_MAX_TEMP);

// //   if (isClimateCrash) {
// //     moldRisk = true;
// //     moldReasonText = "🚨 NOODTOESTAND: Kas uit de hand gelopen (" + String(kasSmoothedTemp, 1) + "°C)!";
// //   }
// //   else if (vpdTooWet) {
// //     moldRisk = true;
// //     moldReasonText = "⚠️ SCHIMMELRISICO: Kas te klam (VPD " + String(kasVpd, 2) + " < " + String(VPD_MIN_OPTIMAL, 2) + " kPa)";
// //   }
// //   else if (vpdTooDry) {
// //     moldRisk = false; 
// //     String cause = lampsAreOn ? "door lampwarmte" : "door omgevingswarmte/droogte";
// //     moldReasonText = "⚠️ UITDROGINGSRISICO: Kas te droog " + cause + " (VPD " + String(kasVpd, 2) + " kPa)";
// //   }
// //   else if (condensRisk) {
// //     moldRisk = true;
// //     moldReasonText = "⚠️ KRITIEK: Condensgevaar op zaadjes! Marge: " + String(kasDpMargin, 1) + "°C";
// //   } 
// //   else if (humHigh) {
// //     moldRisk = true;
// //     moldReasonText = "⚠️ RISICO: Luchtvochtigheid te hoog (" + String(kasSmoothedHum, 1) + "%)";
// //   } 
// //   else if (tempTooLow) {
// //     moldRisk = false;
// //     moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te laag (" + String(kasSmoothedTemp, 1) + "°C)";
// //   }
// //   else if (tempTooHigh) {
// //     moldRisk = false;
// //     String cause = lampsAreOn ? "door verlichting" : "door externe warmte";
// //     moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te hoog " + cause + " (" + String(kasSmoothedTemp, 1) + "°C)";
// //   }
// //   else {
// //     moldRisk = false;
// //     moldReasonText = "✅ Ideaal kiemklimaat";
// //   }

// //   if (isClimateCrash || tempTooHigh) {
// //     statusLed.setColor(255, 0, 0); 
// //   } 
// //   else if (vpdTooDry || vpdTooWet || condensRisk) {
// //     statusLed.setColor(255, 100, 0); 
// //   }
// //   else {
// //     statusLed.setColor(0, 0, 0); 
// //   }

// //   if (isClimateCrash) vpdStatusText = " `(🚨 NOODSTOP)`";
// //   else if (vpdTooWet) vpdStatusText = " `(⚠️ Te klam)`";
// //   else if (vpdTooDry) vpdStatusText = " `(🔥 Te droog)`";
// //   else vpdStatusText = " `(✅ Optimaal)`";

// //   if (condensRisk) dpMarginStatusText = " `(⚠️ Risico)`";
// //   else dpMarginStatusText = " `(✅ Veilig)`";

// //   if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum)) {
// //     indoorDewPoint = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
// //     indoorVpd = calcVPD(indoorSmoothedTemp, indoorSmoothedHum);
// //     indoorDpMargin = indoorSmoothedTemp - indoorDewPoint;
// //   }

// //   if (!isnan(outdoorTemp) && !isnan(outdoorHumidity)) {
// //     if (outdoorHumidity <= 0.0 || outdoorHumidity > 100.0 || outdoorTemp <= -900.0) {
// //       outdoorDewPoint = -999.0;
// //       outdoorVpd = -999.0;
// //       outdoorDpMargin = -999.0;
// //     } else {
// //       outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
// //       outdoorVpd = calcVPD(outdoorTemp, outdoorHumidity);
// //       outdoorDpMargin = outdoorTemp - outdoorDewPoint; 
// //     }
// //   }
// // }

// // void applySelfLearning(float currentTemp, float targetTemp, int &fanSpeed) {
// //   unsigned long now = millis();

// //   if (lastFanSpeedCommand > 0 && fanSpeed == 0) {
// //     float tempDelta = currentTemp - tempBeforeFanAction;
// //     if (abs(tempDelta) > 0.1f) {
// //       learnedThermalInertia = (1.0f - 0.05f) * learnedThermalInertia + 0.05f * abs(tempDelta);
// //       logToSyslogAndSerialPrintf("[AI LEARN] Geleerde thermische traagheid (Inertia): %.2f", learnedThermalInertia);
// //     }
// //   }

// //   if (lastFanSpeedCommand == 0 && fanSpeed > 0) {
// //     tempBeforeFanAction = currentTemp;
// //   }
// //   lastFanSpeedCommand = fanSpeed;

// //   bool currentTrendUp = (currentTemp > targetTemp + dynamicSetpointOffset);
// //   static bool previousTrendUp = false;

// //   if (currentTrendUp != previousTrendUp) {
// //     if (now - lastToggleTime < 120000UL) { 
// //       stateToggleCount++;
// //       if (stateToggleCount >= 6) { 
// //         dynamicSetpointOffset += (currentTrendUp ? 0.05f : -0.05f);
// //         stateToggleCount = 0;
// //         logToSyslogAndSerialPrintf("[AI LEARN] Oscillatie gedetecteerd. Dynamische setpoint offset aangepast naar: %.2f°C", dynamicSetpointOffset);
// //       }
// //     } else {
// //       stateToggleCount = 0; 
// //     }
// //     lastToggleTime = now;
// //     previousTrendUp = currentTrendUp;
// //   }
// // }

// // void calculateAdaptiveClimate() {
// //   unsigned long now = millis();
// //   float deltaTime = (now - lastPidTime) / 1000.0f;
// //   if (deltaTime < 1.0f) return; 
// //   lastPidTime = now;

// //   if (isClimateCrash) {
// //     fanIntSpeed = 255;
// //     fanExt1Speed = 255;
// //     ledcWrite(PIN_FAN_INT_PWM, 255);
// //     ledcWrite(PIN_FAN_EXT1_PWM, 255);
// //     return;
// //   }

// //   // =========================================================================
// //   // Slimmer Woning vs Buiten Advies (15-min Lookahead, Fysische Hiërarchie & Escalatie)
// //   // =========================================================================
// //   static HouseVentState activeAdviceState = HOUSE_CLOSED;
// //   static bool pendingEscalationCheck = false;
// //   static unsigned long escalationTimer = 0;
// //   static float baselineIndoorHum = -999.0f;
// //   static float baselineKasVpd = -999.0f;

// //   if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum) && !isnan(outdoorTemp) && outdoorHumidity > 0.0f) {
    
// //     // 1. Fysische Dauwpuntvergelijking (Kas -> Woning -> Buiten)
// //     // Omdat de kas binnenshuis staat, test de kas eerst op de woning, en de woning op buiten.
// //     float inDp  = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
// //     float outDp = calcDewPoint(outdoorTemp, outdoorHumidity);

// //     bool kasIsDumpingMoisture = (fanExt1Speed > 0 || kasSmoothedHum > 75.0f || kasVpd > VPD_MAX_OPTIMAL || kasVpd < VPD_MIN_OPTIMAL);
// //     bool houseTooHumid = (indoorSmoothedHum > 62.0f);
    
// //     // Buitenlucht moet significant droger zijn (minstens 1.5°C in dauwpunt) om effectief te luchten
// //     bool outsideAirIsSignificantlyDrier = (outDp < (inDp - 1.5f));

// //     // 2. 3-uurs API voorspelling & trend check
// //     bool forecastBadWeather = (outdoorHumidity > 85.0f || forecastedTempSoon < 0.0f);

// //     HouseVentState desiredAdvice = HOUSE_CLOSED;
// //     String newReason = "";

// //     // Bepaal gewenste advies op basis van huidige status + 15-minutentrend (velocity)
// //     float projectedIndoorHum = indoorSmoothedHum + (humidityVelocity * 15.0f);

// //     if ((houseTooHumid || kasIsDumpingMoisture || projectedIndoorHum > 63.0f) && outsideAirIsSignificantlyDrier && !forecastBadWeather) {
// //       desiredAdvice = HOUSE_VENTILATE;
// //       if (kasIsDumpingMoisture && houseTooHumid) {
// //         newReason = "Kas dumpt vocht & woning klam. Buitenlucht is gunstig; ventileren aanbevolen.";
// //       } else if (kasIsDumpingMoisture) {
// //         newReason = "Kas ventileert naar woning; buitenlucht is droger, luchten zinvol.";
// //       } else {
// //         newReason = "Woning te klam (of stijgende trend); buitenlucht is droger.";
// //       }
// //     } else {
// //       desiredAdvice = HOUSE_CLOSED;
// //       if (forecastBadWeather) {
// //         newReason = "Woning/Kas in balans, maar naderend vocht/weeromslag volgens API-voorspelling.";
// //       } else if (!outsideAirIsSignificantlyDrier) {
// //         newReason = "Woning/Kas in balans (buitenlucht biedt geen voldoende dauwpunt-marge).";
// //       } else {
// //         newReason = "Woningklimaat stabiel; woningbuffer heeft voldoende capaciteit voor de kas (luchten niet nodig).";
// //         // newReason = "Woningklimaat stabiel.";
// //       }
// //     }

// //     // 3. Traagheids- en Evaluatiemechanisme (15 Minuten Venster)
// //     unsigned long evalNow = millis();
// //     if (desiredAdvice != activeAdviceState) {
// //       if (!pendingEscalationCheck) {
// //         // Start het 15-minuten observatievenster
// //         pendingEscalationCheck = true;
// //         escalationTimer = evalNow;
// //         baselineIndoorHum = indoorSmoothedHum;
// //         baselineKasVpd = kasVpd;
// //         logToSyslogAndSerialPrintf("[HOUSE ADVICE] Klimaatverschuiving gedetecteerd. Start 15-minuten evaluatievenster.");
// //       } else if (evalNow - escalationTimer >= 900000UL) { // 15 minuten = 900.000 ms
// //         // Na 15 minuten: evalueer de werkelijke richtingsverandering
// //         bool worsening = (indoorSmoothedHum > baselineIndoorHum + 1.0f) || (kasVpd > baselineKasVpd + 0.15f);
// //         bool improving = (indoorSmoothedHum < baselineIndoorHum) || (kasVpd < baselineKasVpd);

// //         if (worsening) {
// //           // Escalatie: het klimaat blijft de foute kant op gaan!
// //           String escMsg = "⚠️ *ESCALATIE KLIMAAT:* Woning/Kas vochtigheid/VPD blijft oplopen na 15 min! Extra maatregelen vereist.";
// //           logToSyslogAndSerialPrintf("[HOUSE ADVICE ESCALATION] Klimaat blijft escaleren na 15 minuten.");
// //           sendTelegramAlert(escMsg);
// //         } else if (improving) {
// //           // Succesbevestiging: ventileren werkt, meld dat het op tijd weer dicht kan
// //           String succMsg = "✅ *SUCCESBEVESTIGING:* Ventileren van de woning is effectief; klimaat stabiliseert positief. Bereid sluiting voor.";
// //           logToSyslogAndSerialPrintf("[HOUSE ADVICE SUCCESS] Ventileren effectief, klimaat stabiliseert.");
// //           sendTelegramAlert(succMsg);
// //         }

// //         activeAdviceState = desiredAdvice;
// //         houseAdvice = desiredAdvice;
// //         houseAdviceReason = newReason;
// //         pendingEscalationCheck = false; // reset
// //       }
// //     } else {
// //       pendingEscalationCheck = false;
// //       houseAdvice = desiredAdvice;
// //       houseAdviceReason = newReason;
// //     }
// //   }

// //   float targetTemp = 22.0f + dynamicSetpointOffset; 
// //   float currentTemp = kasSmoothedTemp;
// //   float error = currentTemp - targetTemp;
// //   pError = error;

// //   float Kp = (abs(error) > 2.0f) ? 18.0f : 8.0f; 
// //   float Ki = 0.3f;
// //   float Kd = 2.0f;

// //   iError += error * deltaTime;
// //   if (iError > 30.0f) iError = 30.0f;
// //   if (iError < -30.0f) iError = -30.0f;

// //   dError = (error - lastError) / deltaTime;
// //   lastError = error;

// //   float pidOutput = (Kp * pError) + (Ki * iError) + (Kd * dError);

// //   if (isLampTransitionActive) {
// //     unsigned long transitionElapsed = now - lampTransitionStartTime;
// //     if (transitionElapsed < 600000UL) pidOutput += 10.0f; 
// //     else isLampTransitionActive = false; 
// //   }

// //   int rawPidOutput = (int)pidOutput;

// //   // =========================================================================
// //   // Interne fan (circulatie)
// //   // =========================================================================
// //   int finalIntSpeed = 0;
// //   if (moldRisk || kasSmoothedHum > HUM_MOLD_THRESHOLD || rawPidOutput > -5.0f) {
// //     if (fanIntSpeed == 0) finalIntSpeed = INT_FAN_BASE_PWM;
// //     else finalIntSpeed = max(INT_FAN_MIN_PWM, min(rawPidOutput, 255));
// //   } else {
// //     finalIntSpeed = 0;
// //   }

// //   // =========================================================================
// //   // Externe fans (Proportioneel + Asymmetrisch uitrollen)
// //   // =========================================================================
// //   int targetExt1Speed = 0;
// //   if (error > 0.2f || rawPidOutput > 30.0f) {
// //     // Proportionele schaling op basis van de PID-output
// //     float factor = (pidOutput - 30.0f) / 100.0f;
// //     if (factor < 0.0f) factor = 0.0f;
// //     if (factor > 1.0f) factor = 1.0f;
    
// //     targetExt1Speed = EXT_FAN_BASE_PWM + (int)(factor * (FAN_MAX_PWM - EXT_FAN_BASE_PWM));
// //     if (targetExt1Speed < EXT_FAN_MIN_PWM) targetExt1Speed = EXT_FAN_MIN_PWM;
// //     if (targetExt1Speed > FAN_MAX_PWM) targetExt1Speed = FAN_MAX_PWM;
// //   } else {
// //     targetExt1Speed = 0;
// //   }

// //   // Asymmetrische filtering: direct opschalen, langzaam afschalen
// //   int finalExt1Speed = fanExt1Speed;
// //   if (targetExt1Speed > finalExt1Speed) {
// //     // Direct opschalen voor een snelle correctie bij oplopende VPD/warmte
// //     finalExt1Speed = targetExt1Speed;
// //   } else if (targetExt1Speed < finalExt1Speed) {
// //     // Geleidelijk afschalen (langzaam uitrollen) om abrupte stops en jojoën te voorkomen
// //     finalExt1Speed -= 3; // Verlaagt de PWM geleidelijk per regelcyclus
// //     if (finalExt1Speed < EXT_FAN_MIN_PWM && finalExt1Speed > 0) {
// //       finalExt1Speed = 0; // Harde val naar 0 om de dode zone (1 t/m MIN_PWM) te vermijden
// //     }
// //     if (targetExt1Speed == 0 && finalExt1Speed < EXT_FAN_MIN_PWM) {
// //       finalExt1Speed = 0;
// //     }
// //   }

// //   applySelfLearning(currentTemp, targetTemp, finalExt1Speed);

// //   fanIntSpeed = finalIntSpeed;
// //   fanExt1Speed = finalExt1Speed;

// //   ledcWrite(PIN_FAN_INT_PWM, fanIntSpeed);
// //   ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
// // }

// // void calculateTrayClimate() {
// //   if (tray1Mode != 3) { 
// //     float targetMin1 = getTrayMinTemp(tray1Mode);
// //     float targetMax1 = getTrayMaxTemp(tray1Mode);
// //     float currentTray1 = traySensor1Connected ? traySensor1Temp : targetMin1;
// //     (void)targetMin1; (void)targetMax1; (void)currentTray1;
// //   }
// //   if (tray2Mode != 3) {
// //     float targetMin2 = getTrayMinTemp(tray2Mode);
// //     float targetMax2 = getTrayMaxTemp(tray2Mode);
// //     float currentTray2 = traySensor2Connected ? traySensor2Temp : targetMin2;
// //     (void)targetMin2; (void)targetMax2; (void)currentTray2;
// //   }
// // }

// // void updateHeatingAdvice(float inTemp, float outTemp) {
// //   if (isnan(inTemp) || isTestModeActive || isKasSleeping) return; 
// //   (void)outTemp;
// //   if (inTemp > HEAT_MAT_DISABLE_TEMP) {
// //     if (isHeatMatRecommended) logToSerial("[HEATING] Kas temperatuur boven grenswaarde, warmtemat uitgeschakeld.");
// //     isHeatMatRecommended = false;
// //     heatMatLowStartTime = 0;
// //     heatMatHighStartTime = 0;
// //     return;
// //   }
// //   if (inTemp < HEAT_MAT_TEMP_LOW) {
// //     heatMatHighStartTime = 0; 
// //     if (!isHeatMatRecommended) { 
// //       if (heatMatLowStartTime == 0) heatMatLowStartTime = millis(); 
// //       else if (millis() - heatMatLowStartTime >= HEAT_MAT_DELAY) {
// //         logToSyslogAndSerialPrintf("[HEATING] Kas temperatuur te laag (%.1f°C), warmtemat AANbevolen.", inTemp);
// //         isHeatMatRecommended = true; 
// //         heatMatLowStartTime = 0;     
// //       }
// //     }
// //   } else if (inTemp > HEAT_MAT_TEMP_HIGH) {
// //     heatMatLowStartTime = 0; 
// //     if (isHeatMatRecommended) { 
// //       if (heatMatHighStartTime == 0) heatMatHighStartTime = millis(); 
// //       else if (millis() - heatMatHighStartTime >= HEAT_MAT_DELAY) {
// //         logToSyslogAndSerialPrintf("[HEATING] Kas temperatuur OK (%.1f°C), warmtemat UITgeschakeld.", inTemp);
// //         isHeatMatRecommended = false; 
// //         heatMatHighStartTime = 0;     
// //       }
// //     }
// //   } else {
// //     heatMatLowStartTime = 0;
// //     heatMatHighStartTime = 0;
// //   }
// // }

// // void checkClimateVelocity(float currentHum) {
// //   if (millis() - lastVelocityCheckTime >= CLIMATE_CHECK_INTERVAL_MS || lastVelocityCheckTime == 0) {
// //     if (previousHumForVelocity != -999.0) {
// //       humidityVelocity = currentHum - previousHumForVelocity; 
// //       if (humidityVelocity >= HUMIDITY_VELOCITY_THRESHOLD && currentHum > HUMIDITY_VELOCITY_MIN_HUM) {
// //         logToSyslogAndSerialPrintf("[ALERT] Snelle vochtigheidstoename in kas: +%.1f%%/min", humidityVelocity);
// //         kasAdvice = GREENHOUSE_CIRCULATE_INTERNAL; 
// //       }
// //     }
// //     previousHumForVelocity = currentHum;
// //     lastVelocityCheckTime = millis();
// //   }
// // }

// // void updateMoldRiskHistory(bool currentRisk) {
// //   if (millis() - lastMoldSampleTime >= CLIMATE_CHECK_INTERVAL_MS || lastMoldSampleTime == 0) {
// //     lastMoldSampleTime = millis();
// //     moldHistory[moldSampleIndex] = currentRisk;
// //     moldSampleIndex = (moldSampleIndex + 1) % MOLD_SAMPLES;
// //     if (moldSampleIndex == 0) moldHistoryFilled = true;

// //     int positiveCount = 0;
// //     int total = moldHistoryFilled ? MOLD_SAMPLES : moldSampleIndex;
// //     for (int i = 0; i < total; i++) { if (moldHistory[i]) positiveCount++; }

// //     bool filteredMoldRisk = (positiveCount > (total / 2));
// //     if (filteredMoldRisk != previousMoldRisk) {
// //       previousMoldRisk = filteredMoldRisk;
// //       logToSyslogAndSerial(filteredMoldRisk ? "[MOLD ALERT] Schimmelrisico gedetecteerd!" : "[MOLD RECOVERY] Schimmelrisico geweken.");
// //     }
// //   }
// // }

// // void checkKasTrends(float currentTemp, float currentHum, float currentBaro) {
// //   if (pastBaro < 0) { pastBaro = currentBaro; lastTrendSample = millis(); return; }
// //   (void)currentTemp; (void)currentHum;
// //   if (millis() - lastTrendSample > CLIMATE_CHECK_INTERVAL_MS) {
// //     float baroDiff = currentBaro - pastBaro;
// //     if (baroDiff >= BARO_TREND_THRESHOLD) baroTrendArrow = "^";
// //     else if (baroDiff <= -BARO_TREND_THRESHOLD) baroTrendArrow = "v";
// //     else baroTrendArrow = "=";
// //     pastBaro = currentBaro;
// //     lastTrendSample = millis();
// //   }
// // }

// // void updateHighLow(float currentTemp) {
// //   if (isnan(currentTemp) || currentTemp <= -999.0f) return;
// //   if (currentTemp > kasTempHigh) kasTempHigh = currentTemp;
// //   if (currentTemp < kasTempLow) kasTempLow = currentTemp;
// // }

// // void updateHumidityHighLow(float kasHum, float indoorHum, float outHum) { 
// //   (void)kasHum; (void)indoorHum; (void)outHum; 
// // }


