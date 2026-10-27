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

// --- ZELFLERENDE LAMPCORRECTIE ---
float learnedLampTempOffset = 2.0f; // Startwaarde als veilige fallback
static float tempBeforeLampsOn = 0.0f;
static unsigned long lampOnTimestamp = 0;
static bool lampLearningLocked = false;

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

// ================================================
// Zelflerende lampcorrectie
// ================================================
void applyLampLearning(bool currentLampsOn, float kasTemp) {
  static bool prevLampsState = false;
  unsigned long now = millis();

  // Lampen springen AAN: sla de beginsituatie op
  if (currentLampsOn && !prevLampsState) {
    tempBeforeLampsOn = kasTemp;
    lampOnTimestamp = now;
    lampLearningLocked = false;
  }
  
  // Meet na 30 minuten branden de daadwerkelijk opgetreden temperatuurstijging
  if (currentLampsOn && !lampLearningLocked && (now - lampOnTimestamp >= 1800000UL)) { // 30 min
    float actualTempDelta = kasTemp - tempBeforeLampsOn;
    // Valideer of de delta realistisch is (tussen 0.5°C en 4.0°C) om uitschieters te voorkomen
    if (actualTempDelta > 0.5f && actualTempDelta < 4.0f) {
      learnedLampTempOffset = (1.0f - 0.1f) * learnedLampTempOffset + 0.1f * actualTempDelta;
      logToSyslogAndSerialPrintf("[AI LEARN] Geleerde lamp temperatuur offset bijgesteld naar: %.2f°C (gemeten delta: %.2f°C)", learnedLampTempOffset, actualTempDelta);
    }
    lampLearningLocked = true; // Slechts eenmalig per brandcyclus bijstellen
  }

  prevLampsState = currentLampsOn;
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
  if (lampsAreOn && !prevLampsAreOn) {
    lastLightsOnMillis = millis();
    hasStoredLastLightsOn = true;
    logToSyslogAndSerialPrintf("[FEEDFORWARD] Lampen AAN gedetecteerd. Cyclus-start opgeslagen voor 24-uurs anticipatie.");
  }
  prevLampsAreOn = lampsAreOn;

  int minutesUntilLightsOn = testMinutesUntilLightsOn; 
  
  if (hasStoredLastLightsOn && !lampsAreOn) {
    unsigned long elapsedSinceLastOn = millis() - lastLightsOnMillis;
    unsigned long cycleDurationMs = 86400000UL; // 24 uur in milliseconden
    
    if (elapsedSinceLastOn < cycleDurationMs) {
      unsigned long remainingMs = cycleDurationMs - elapsedSinceLastOn;
      minutesUntilLightsOn = remainingMs / 60000UL; 
    } else {
      minutesUntilLightsOn = 0; 
    }
  } else if (lampsAreOn) {
    minutesUntilLightsOn = 0; 
  }

  currentMinutesUntilLightsOn = minutesUntilLightsOn;

  upcomingLightsOn = (!lampsAreOn && minutesUntilLightsOn <= PREEMPTIVE_LAMP_LOOKAHEAD_MINUTES && minutesUntilLightsOn > 0);
  isPreemptiveLampConditioningActive = upcomingLightsOn;

  float targetIndoorVpdMin = 0.80f;

  if (upcomingLightsOn) {
    targetIndoorVpdMin = 0.95f; 
  } else if (learnedClimateBias < -0.3f) {
    targetIndoorVpdMin = 0.90f; 
  } else if (learnedClimateBias > 0.3f) {
    targetIndoorVpdMin = 0.75f; 
  }

  // =========================================================================
  // VERRUIMDE & LAMP-AWARE DYNAMISCHE VPD GRENZEN (Rustig & Realistisch)
  // =========================================================================
  // =========================================================================
  // VERRUIMDE & LAMP-AWARE DYNAMISCHE VPD GRENZEN (Rustig & Realistisch)
  // =========================================================================
  float currentMaxOptimalVpd = lampsAreOn ? VPD_MAX_OPTIMAL_DAY : VPD_MAX_OPTIMAL_NIGHT; 
  float currentMinOptimalVpd = lampsAreOn ? VPD_MIN_OPTIMAL_DAY : VPD_MIN_OPTIMAL_NIGHT; 

  bool vpdTooWet   = (kasVpd < currentMinOptimalVpd);
  bool vpdTooDry   = (kasVpd > currentMaxOptimalVpd);
  bool humHigh     = (kasSmoothedHum >= HUM_MOLD_THRESHOLD);
  bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
  bool tempTooLow  = (kasSmoothedTemp < HEAT_MAT_TEMP_LOW);
  bool tempTooHigh = (kasSmoothedTemp > GREENHOUSE_MAX_TEMP);
  
  // Bepaal of de kas actief bezig is met temperatuurcorrectie (warmte afvoeren)
  float effectiveTargetTemp = lampsAreOn ? 24.0f : 22.0f;
  bool tempCorrectionActive = (kasSmoothedTemp > effectiveTargetTemp + 0.4f && !tempTooHigh);

  if (isClimateCrash) {
    moldRisk = true;
    moldReasonText = "🚨 NOODTOESTAND: Kas uit de hand gelopen (" + String(kasSmoothedTemp, 1) + "°C)!";
  }
  else if (condensRisk) {
    moldRisk = true;
    moldReasonText = "⚠️ KRITIEK: Condensgevaar op zaadjes! Marge: " + String(kasDpMargin, 1) + "°C";
  } 
  else if (vpdTooWet || humHigh) {
    moldRisk = false; // Geen paniek-alarm; dit is Categorie 2 (ESP regelt vocht bij)
    moldReasonText = "💧 Vochtcorrectie actief (buffer regelt bij, VPD: " + String(kasVpd, 2) + " kPa)";
  }
  else if (tempCorrectionActive) {
    moldRisk = false;
    moldReasonText = "🌡️ Temperatuurcorrectie actief (warmte-afvoer, Temp: " + String(kasSmoothedTemp, 1) + "°C)";
  }
  else if (upcomingLightsOn) {
    moldRisk = false;
    moldReasonText = "🔮 Lamp-anticipatie actief (woningbuffer wordt voorbereid)";
  }
  else if (vpdTooDry) {
    moldRisk = false; 
    String cause = lampsAreOn ? "door lampwarmte (geaccepteerd)" : "door omgevingswarmte/droogte";
    moldReasonText = "🔥 Lichte uitdroging " + cause + " (VPD " + String(kasVpd, 2) + " kPa)";
  }
  else if (tempTooLow) {
    moldRisk = false;
    moldReasonText = "⚠️ Kiemtemperatuur aan de lage kant (" + String(kasSmoothedTemp, 1) + "°C)";
  }
  else if (tempTooHigh) {
    moldRisk = false;
    String cause = lampsAreOn ? "door verlichting" : "door externe warmte";
    moldReasonText = "⚠️ Kiemtemperatuur aan de hoge kant " + cause + " (" + String(kasSmoothedTemp, 1) + "°C)";
  }
  else {
    moldRisk = false;
    moldReasonText = lampsAreOn ? "✅ Ideaal kiemklimaat (inclusief lamp-compensatie)" : "✅ Ideaal kiemklimaat en stabiele buffer";
  }

  // LED sturing: ook oranje bij actieve temperatuurcorrectie
  if (isClimateCrash || tempTooHigh) {
    statusLed.setColor(255, 0, 0); // Rood
  } 
  else if (vpdTooDry || condensRisk || tempCorrectionActive) {
    statusLed.setColor(255, 100, 0); // Oranje
  }
  else {
    statusLed.setColor(0, 0, 0); // Uit / Groen (Normale werking)
  }

  if (isClimateCrash) vpdStatusText = " `🚨 NOODSTOP`";
  else if (vpdTooWet) vpdStatusText = " `💧 Vochtcorrectie`";
  else if (tempCorrectionActive) vpdStatusText = " `🌡️ Temp-correctie`";
  else if (vpdTooDry) vpdStatusText = " `🔥 Te droog`";
  else vpdStatusText = " `✅ Optimaal`";

  if (condensRisk) dpMarginStatusText = " `⚠️ Risico`";
  else dpMarginStatusText = " `✅ Veili)`";

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

    // Gebruik lamp-aware grenswaarden
    float maxOpt = lampsAreOn ? VPD_MAX_OPTIMAL_DAY : VPD_MAX_OPTIMAL_NIGHT;
    float minOpt = lampsAreOn ? VPD_MIN_OPTIMAL_DAY : VPD_MIN_OPTIMAL_NIGHT;

    if (currentVpd > maxOpt) {
      learnedClimateBias += 0.05f;
      if (learnedClimateBias > 1.0f) learnedClimateBias = 1.0f;
    } else if (currentVpd < minOpt) {
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

    // ✅ Opgelost: Hier definieren we maxOpt en minOpt zodat ze binnen scope zijn
    float maxOpt = lampsAreOn ? VPD_MAX_OPTIMAL_DAY : VPD_MAX_OPTIMAL_NIGHT;
    float minOpt = lampsAreOn ? VPD_MIN_OPTIMAL_DAY : VPD_MIN_OPTIMAL_NIGHT;

    bool kasIsDumpingMoisture = (fanExt1Speed > 0 || kasSmoothedHum > 75.0f || kasVpd > maxOpt || kasVpd < minOpt);

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
        newReason = "Woning & Kas in balans; buitenlucht biedt te weinig dauwpunt-marge voor woningbuffer.";
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
  // PID & VENTILATOR STURING (Met Hysteresis, Vloeiende Smoothing & AI Lamp Learning)
  // =========================================================================
  
  // Voer de zelflerende lampcorrectie uit
  applyLampLearning(lampsAreOn, kasSmoothedTemp);

  // Gebruik de dynamisch geleerde offset in plaats van een harde 2.0f
  float lampTemperatureOffset = lampsAreOn ? learnedLampTempOffset : 0.0f;
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

  // =========================================================================
  // 1. INTERNE FAN (Circulatie) - Met hysteresis en ~5s vloeiende ramp-up/down
  // =========================================================================
  bool internalFanNeeded = (moldRisk || kasSmoothedHum > 78.0f || rawPidOutput > 0.0f || isPreemptiveLampConditioningActive);
  static bool internalFanActiveState = false;
  
  // Hysteresis: pas uitschakelen als vocht en PID weer netjes zijn gezakt
  if (internalFanNeeded) {
    internalFanActiveState = true;
  } else if (kasSmoothedHum < 75.0f && rawPidOutput < -5.0f && !moldRisk) {
    internalFanActiveState = false;
  }

  int targetIntSpeed = 0;
  if (internalFanActiveState) {
    targetIntSpeed = max(INT_FAN_MIN_PWM, min(rawPidOutput, 255));
    if (targetIntSpeed < INT_FAN_BASE_PWM) targetIntSpeed = INT_FAN_BASE_PWM;
  } else {
    targetIntSpeed = 0;
  }

  int finalIntSpeed = fanIntSpeed;
  if (targetIntSpeed > finalIntSpeed) {
    finalIntSpeed += 25; // Snelle, vloeiende opstart in ca. 5 stappen (~5 sec)
    if (finalIntSpeed > targetIntSpeed) finalIntSpeed = targetIntSpeed;
  } else if (targetIntSpeed < finalIntSpeed) {
    finalIntSpeed -= 25; // Vloeiend afrollen
    if (finalIntSpeed < INT_FAN_MIN_PWM) {
      finalIntSpeed = 0;
    }
  }
  if (targetIntSpeed == 0 && finalIntSpeed < INT_FAN_MIN_PWM) {
    finalIntSpeed = 0;
  }

  // =========================================================================
  // 2. EXTERNE FANS - Met hysteresis om 'hunting' rond stabiele setpoints te voorkomen
  // =========================================================================
  static bool extFanActiveState = false;
  if (error > 0.3f || rawPidOutput > 35.0f) {
    extFanActiveState = true;
  } else if (error < 0.0f && rawPidOutput < 15.0f) {
    extFanActiveState = false;
  }

  int targetExt1Speed = 0;
  if (extFanActiveState) {
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
    finalExt1Speed -= 3; // Asymmetrisch rustig uitrollen
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
}
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

//   applyClimateBiasLearning(kasVpd);

//   // =========================================================================
//   // Slimmer Woning vs Buiten Advies
//   // =========================================================================
//   static HouseVentState activeAdviceState = HOUSE_CLOSED;
//   static bool pendingEscalationCheck = false;
//   static unsigned long escalationTimer = 0;
//   static float baselineIndoorHum = -999.0f;
//   static float baselineKasVpd = -999.0f;

//   if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum) && !isnan(outdoorTemp) && outdoorHumidity > 0.0f) {
    
//     float inDp  = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
//     float outDp = calcDewPoint(outdoorTemp, outdoorHumidity);

//     bool kasIsDumpingMoisture = (fanExt1Speed > 0 || kasSmoothedHum > 75.0f || kasVpd > maxOpt || kasVpd < minOpt);

//     bool houseTooHumid = (indoorSmoothedHum > 62.0f);
    
//     float targetIndoorVpdMin = 0.80f;

//     if (learnedClimateBias < -0.3f) {
//       targetIndoorVpdMin = 0.90f; 
//     } else if (learnedClimateBias > 0.3f) {
//       targetIndoorVpdMin = 0.75f; 
//     }

//     bool houseVpdTooLow = (!isnan(indoorVpd) && indoorVpd < targetIndoorVpdMin); 

//     bool outsideAirIsSignificantlyDrier = (outDp < (inDp - 1.5f));
//     bool forecastBadWeather = (outdoorHumidity > 85.0f || forecastedTempSoon < 0.0f);

//     HouseVentState desiredAdvice = HOUSE_CLOSED;
//     String newReason = "";

//     float projectedIndoorHum = indoorSmoothedHum + (humidityVelocity * 15.0f);

//     if ((houseTooHumid || houseVpdTooLow || kasIsDumpingMoisture || projectedIndoorHum > 63.0f) && outsideAirIsSignificantlyDrier && !forecastBadWeather) {
//       desiredAdvice = HOUSE_VENTILATE;
//       if (upcomingLightsOn && houseVpdTooLow) {
//         newReason = "🔮 ANTICIPATIE: Lampen gaan binnenkort aan. Optimaliseer woningbuffer alvast (buitenlucht is droger).";
//       } else if (houseVpdTooLow) {
//         newReason = "Woning-VPD te laag (buffer raakt vol; optimaliseer uitwisseling); buitenlucht is droger.";
//       } else if (kasIsDumpingMoisture && houseTooHumid) {
//         newReason = "Kas dumpt vocht & woning klam. Buitenlucht is gunstig; ventileer woning.";
//       } else {
//         newReason = "Woning te klam of stijgende trend; buitenlucht is droger om buffer te herstellen.";
//       }
//     } else {
//       desiredAdvice = HOUSE_CLOSED;
//       if (forecastBadWeather) {
//         newReason = "Woning & Kas in balans; naderend vocht/weeromslag volgens API-voorspelling.";
//       } else if (!outsideAirIsSignificantlyDrier) {
//         newReason = "Woning & Kas in balans; buitenlucht biedt te weinig dauwpunt-marge voor woningbuffer.";
//       } else {
//         newReason = "Woningklimaat stabiel; buffer heeft voldoende capaciteit voor de kas.";
//       }
//     }

//     unsigned long evalNow = millis();
//     if (desiredAdvice != activeAdviceState) {
//       if (!pendingEscalationCheck) {
//         pendingEscalationCheck = true;
//         escalationTimer = evalNow;
//         baselineIndoorHum = indoorSmoothedHum;
//         baselineKasVpd = kasVpd;
//         logToSyslogAndSerialPrintf("[HOUSE ADVICE] Klimaatverschuiving of buffer-actie gedetecteerd. Start 15-minuten evaluatievenster.");
//       } else if (evalNow - escalationTimer >= 900000UL) { 
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

//   // =========================================================================
//   // PID & VENTILATOR STURING (Met Hysteresis, Vloeiende Smoothing & AI Lamp Learning)
//   // =========================================================================
  
//   // Voer de zelflerende lampcorrectie uit
//   applyLampLearning(lampsAreOn, kasSmoothedTemp);

//   // Gebruik de dynamisch geleerde offset in plaats van een harde 2.0f
//   float lampTemperatureOffset = lampsAreOn ? learnedLampTempOffset : 0.0f;
//   float targetTemp = 22.0f + dynamicSetpointOffset + lampTemperatureOffset; 
  
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

//   // =========================================================================
//   // 1. INTERNE FAN (Circulatie) - Met hysteresis en ~5s vloeiende ramp-up/down
//   // =========================================================================
//   bool internalFanNeeded = (moldRisk || kasSmoothedHum > 78.0f || rawPidOutput > 0.0f || isPreemptiveLampConditioningActive);
//   static bool internalFanActiveState = false;
  
//   // Hysteresis: pas uitschakelen als vocht en PID weer netjes zijn gezakt
//   if (internalFanNeeded) {
//     internalFanActiveState = true;
//   } else if (kasSmoothedHum < 75.0f && rawPidOutput < -5.0f && !moldRisk) {
//     internalFanActiveState = false;
//   }

//   int targetIntSpeed = 0;
//   if (internalFanActiveState) {
//     targetIntSpeed = max(INT_FAN_MIN_PWM, min(rawPidOutput, 255));
//     if (targetIntSpeed < INT_FAN_BASE_PWM) targetIntSpeed = INT_FAN_BASE_PWM;
//   } else {
//     targetIntSpeed = 0;
//   }

//   int finalIntSpeed = fanIntSpeed;
//   if (targetIntSpeed > finalIntSpeed) {
//     finalIntSpeed += 25; // Snelle, vloeiende opstart in ca. 5 stappen (~5 sec)
//     if (finalIntSpeed > targetIntSpeed) finalIntSpeed = targetIntSpeed;
//   } else if (targetIntSpeed < finalIntSpeed) {
//     finalIntSpeed -= 25; // Vloeiend afrollen
//     if (finalIntSpeed < INT_FAN_MIN_PWM) {
//       finalIntSpeed = 0;
//     }
//   }
//   if (targetIntSpeed == 0 && finalIntSpeed < INT_FAN_MIN_PWM) {
//     finalIntSpeed = 0;
//   }

//   // =========================================================================
//   // 2. EXTERNE FANS - Met hysteresis om 'hunting' rond stabiele setpoints te voorkomen
//   // =========================================================================
//   static bool extFanActiveState = false;
//   if (error > 0.3f || rawPidOutput > 35.0f) {
//     extFanActiveState = true;
//   } else if (error < 0.0f && rawPidOutput < 15.0f) {
//     extFanActiveState = false;
//   }

//   int targetExt1Speed = 0;
//   if (extFanActiveState) {
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
//     finalExt1Speed -= 3; // Asymmetrisch rustig uitrollen
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
