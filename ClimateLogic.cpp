/**
 * @file ClimateLogic.cpp
 * @brief Zuivere klimaatlogica, fysische formules (VPD/Dauwpunt), NVS Flash Geheugen & Lamp-compensatie voor ESP32-S3.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"
#include "LedManager.h"
#include <Preferences.h>

extern LedManager statusLed;

// Externe variabelen uit Config.cpp die hier worden gebruikt
extern bool isClimateCrash;
extern String houseAdviceReason;

// --- FILE-SCOPE FEEDFORWARD VARIABELEN ---
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
// float learnedClimateBias = 0.0f; 
static unsigned long lastBiasCheckTime = 0;

// 3 uur van te voren licht anticipatie
static int currentMinutesUntilLightsOn = 0;

// --- ZELFLERENDE LAMPCORRECTIE ---
// float learnedLampTempOffset = 2.0f; 
static float tempBeforeLampsOn = 0.0f;
static unsigned long lampOnTimestamp = 0;
static bool lampLearningLocked = false;

// --- ZELFLERENDE STABILISATIETIJD ---
// unsigned long learnedStabilizationTime = 15; 
static unsigned long stabilizationStartTime = 0;
static bool isWaitingForStabilization = false;

// Getter functie
int getMinutesUntilLightsOn() {
  return currentMinutesUntilLightsOn;
}

// ================================================
// NVS FLASH OPSLAG & LADEN
// ================================================
void updateLearnedStabilizationTime(unsigned long newMinutes) {
  preferences.begin("kas-store", false);
  unsigned long currentLearned = preferences.getULong("stabTime", 15);
  unsigned long finalMinutes = (currentLearned * 4 + newMinutes) / 5;
  preferences.putULong("stabTime", finalMinutes);
  preferences.end();
  learnedStabilizationTime = finalMinutes;
  logToSyslogAndSerialPrintf("[AI LEARN] Geleerde stabilisatietijd bijgesteld naar: %lu minuten", finalMinutes);
}

void saveSystemStateToFlash() {
  preferences.begin("kas-store", false); 
  preferences.putULong("lastLightsOn", lastLightsOnMillis);
  preferences.putBool("hasStoredOn", hasStoredLastLightsOn);
  preferences.putFloat("lampOffset", learnedLampTempOffset);
  preferences.putFloat("climateBias", learnedClimateBias);
  preferences.putFloat("setpointOff", dynamicSetpointOffset);
  preferences.putULong("stabTime", learnedStabilizationTime);
  preferences.end();
  logToSyslogAndSerialPrintf("[STORAGE] Systeemstatussen en AI-parameters opgeslagen in NVS Flash.");
}

void loadSystemStateFromFlash() {
  preferences.begin("kas-store", true); 
  lastLightsOnMillis        = preferences.getULong("lastLightsOn", 0);
  hasStoredLastLightsOn     = preferences.getBool("hasStoredOn", false);
  learnedLampTempOffset     = preferences.getFloat("lampOffset", 2.0f);
  learnedClimateBias        = preferences.getFloat("climateBias", 0.0f);
  dynamicSetpointOffset     = preferences.getFloat("setpointOff", 0.0f);
  learnedStabilizationTime  = preferences.getULong("stabTime", 15);
  preferences.end();
  logToSyslogAndSerialPrintf("[STORAGE] Systeemstatussen geladen uit NVS: LampOffset=%.2f, Bias=%.2f, StabTime=%lu", 
                            learnedLampTempOffset, learnedClimateBias, learnedStabilizationTime);
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

  if (currentLampsOn && !prevLampsState) {
    tempBeforeLampsOn = kasTemp;
    lampOnTimestamp = now;
    lampLearningLocked = false;
  }
  
  if (currentLampsOn && !lampLearningLocked && (now - lampOnTimestamp >= 1800000UL)) { 
    float actualTempDelta = kasTemp - tempBeforeLampsOn;
    if (actualTempDelta > 0.5f && actualTempDelta < 4.0f) {
      learnedLampTempOffset = (1.0f - 0.1f) * learnedLampTempOffset + 0.1f * actualTempDelta;
      logToSyslogAndSerialPrintf("[AI LEARN] Geleerde lamp temperatuur offset bijgesteld naar: %.2f°C (gemeten delta: %.2f°C)", learnedLampTempOffset, actualTempDelta);
      saveSystemStateToFlash(); 
    }
    lampLearningLocked = true; 
  }

  prevLampsState = currentLampsOn;
}

void checkClimateCrash(float inTemp) {
  bool criticalTempHigh = (inTemp >= (GREENHOUSE_CRASH_TEMP + GREENHOUSE_CRASH_OFFSET)); 
  bool safeTempRecovery = (inTemp <= GREENHOUSE_CRASH_TEMP);       

  if (!isClimateCrash && criticalTempHigh) {
    isClimateCrash = true;
    logToSyslogAndSerialPrintf("[CRASH] KLIMAAT CRASH GEDETECTEERD! Kas temperatuur is kritiek: %.1f°C", inTemp);
    sendTelegramAlert("*KRITIEK KLIMAATALARM:* De kas is in een noodtoestand beland (Te heet: " + String(inTemp, 1) + "°C)! Systeem forceert maximale ventilatie.");
  } 
  else if (isClimateCrash && safeTempRecovery) {
    isClimateCrash = false;
    logToSyslogAndSerialPrintf("[RECOVERY] Klimaat crash voorbij. Kas temperatuur hersteld: %.1f°C", inTemp);
    sendTelegramAlert("*HERSTEL:* De kas is weer buiten de gevarenzone (Temp: " + String(inTemp, 1) + "°C). Systeem hervat automatische regeling.");
  }
}

void evaluateClimateState(float kasTemp, float kasHum, float indoorTemp, float indoorHum) {
  if (isnan(kasTemp) || isnan(kasHum)) return;

  if (!isSystemReady()) {
    moldRisk = false;
    moldReasonText = "Kalibreert...";
    vpdStatusText = " `Kalibreren...`";
    dpMarginStatusText = " `Kalibreren...`";
    return;
  }

  kasSmoothedTemp = kasTemp;
  kasSmoothedHum  = kasHum;
  displayedTemp   = kasTemp; 

  kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
  kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
  kasDpMargin = kasSmoothedTemp - kasDewPoint;

  checkClimateCrash(kasSmoothedTemp);

  static unsigned long lampOnStartTime = 0;
  static bool waitingForMinRunTime = false;

  if (lampsAreOn) {
    if (!prevLampsAreOn) {
      lampOnStartTime = millis();
      waitingForMinRunTime = true;
    } else if (waitingForMinRunTime && (millis() - lampOnStartTime >= 600000UL)) { 
      lastLightsOnMillis = lampOnStartTime;
      hasStoredLastLightsOn = true;
      waitingForMinRunTime = false;
      saveSystemStateToFlash(); 
      logToSyslogAndSerialPrintf("[FEEDFORWARD] Echte dagcyclus gestart. Starttijd opgeslagen in NVS.");
    }
  } else {
    waitingForMinRunTime = false;
  }
  prevLampsAreOn = lampsAreOn;

  int minutesUntilLightsOn = testMinutesUntilLightsOn; 
  
  if (hasStoredLastLightsOn && !lampsAreOn) {
    unsigned long elapsedSinceLastOn = millis() - lastLightsOnMillis;
    unsigned long cycleDurationMs = 86400000UL; 
    
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

  if (upcomingLightsOn) {
    dynamicSetpointOffset = -0.5f; 
  } else if (!lampsAreOn) {
    dynamicSetpointOffset = 0.0f;
  }

  float currentMaxOptimalVpd = lampsAreOn ? VPD_MAX_OPTIMAL_DAY : VPD_MAX_OPTIMAL_NIGHT; 
  float currentMinOptimalVpd = lampsAreOn ? VPD_MIN_OPTIMAL_DAY : VPD_MIN_OPTIMAL_NIGHT; 

  bool vpdTooWet   = (kasVpd < currentMinOptimalVpd);
  bool vpdTooDry   = (kasVpd > currentMaxOptimalVpd);
  bool humHigh     = (kasSmoothedHum >= HUM_MOLD_THRESHOLD);
  bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
  bool tempTooLow  = (kasSmoothedTemp < HEAT_MAT_TEMP_LOW);
  bool tempTooHigh = (kasSmoothedTemp > GREENHOUSE_MAX_TEMP);
  
  float effectiveTargetTemp = lampsAreOn ? 24.0f : 22.0f;
  bool tempCorrectionActive = (kasSmoothedTemp > effectiveTargetTemp + 0.4f && !tempTooHigh);

  // ZELFLERENDE STABILISATIETIJD METING
  bool hasClimateIssue = (vpdTooWet || vpdTooDry || condensRisk || tempCorrectionActive || moldRisk);
  unsigned long now = millis();

  if (hasClimateIssue && !isWaitingForStabilization) {
    stabilizationStartTime = now;
    isWaitingForStabilization = true;
  } else if (!hasClimateIssue && isWaitingForStabilization) {
    unsigned long durationMinutes = (now - stabilizationStartTime) / 60000UL;
    if (durationMinutes >= 2 && durationMinutes <= 120) {
      updateLearnedStabilizationTime(durationMinutes);
    }
    isWaitingForStabilization = false;
  }

  if (isClimateCrash) {
    moldRisk = true;
    moldReasonText = "NOODTOESTAND: Kas uit de hand gelopen (" + String(kasSmoothedTemp, 1) + "°C)!";
  }
  else if (condensRisk) {
    moldRisk = true;
    moldReasonText = "KRITIEK: Condensgevaar op zaadjes! Marge: " + String(kasDpMargin, 1) + "°C";
  } 
  else if (vpdTooWet || humHigh) {
    moldRisk = false; 
    moldReasonText = "Vochtcorrectie actief (buffer regelt bij, vochtigheid is wat hoog)";
  }
  else if (tempCorrectionActive) {
    moldRisk = false;
    moldReasonText = "Warmte afvoeren (temperatuur in de kas regelt bij)";
  }
  else if (upcomingLightsOn) {
    moldRisk = false;
    moldReasonText = "Lamp-anticipatie actief: we bereiden de binnenbuffer alvast voor op de warmte.";
  }
  else if (vpdTooDry) {
    moldRisk = false; 
    String cause = lampsAreOn ? "door de lampen (normaal)" : "door omgevingswarmte";
    moldReasonText = "Lichte uitdroging " + cause + " (klimaat is onder controle)";
  }
  else if (tempTooLow) {
    moldRisk = false;
    moldReasonText = "Kiemtemperatuur is wat aan de frisse kant (" + String(kasSmoothedTemp, 1) + "°C)";
  }
  else if (tempTooHigh) {
    moldRisk = false;
    String cause = lampsAreOn ? "door verlichting" : "door externe warmte";
    moldReasonText = "Kiemtemperatuur is wat aan de warme kant " + cause + " (" + String(kasSmoothedTemp, 1) + "°C)";
  }
  else {
    moldRisk = false;
    moldReasonText = lampsAreOn ? "Ideaal kiemklimaat (inclusief lamp-compensatie)" : "Ideaal kiemklimaat en stabiele buffer";
  }

  if (isClimateCrash || tempTooHigh) {
    statusLed.setColor(255, 0, 0); 
  } 
  else if (vpdTooDry || condensRisk || tempCorrectionActive) {
    statusLed.setColor(255, 100, 0); 
  }
  else {
    statusLed.setColor(0, 0, 0); 
  }

  if (isClimateCrash) vpdStatusText = " `NOODSTOP`";
  else if (vpdTooWet) vpdStatusText = " `Vochtcorrectie`";
  else if (tempCorrectionActive) vpdStatusText = " `Temp-correctie`";
  else if (vpdTooDry) vpdStatusText = " `Te droog`";
  else vpdStatusText = " `Optimaal`";

  if (condensRisk) dpMarginStatusText = " `Risico`";
  else dpMarginStatusText = " `Veilig`";

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
        saveSystemStateToFlash(); 
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
      saveSystemStateToFlash(); 
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

  static HouseVentState activeAdviceState = HOUSE_CLOSED;
  static bool pendingEscalationCheck = false;
  static unsigned long escalationTimer = 0;
  static float baselineIndoorHum = -999.0f;
  static float baselineKasVpd = -999.0f;

  if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum) && !isnan(outdoorTemp) && outdoorHumidity > 0.0f) {
    
    float inDp  = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
    float outDp = calcDewPoint(outdoorTemp, outdoorHumidity);

    float maxOpt = lampsAreOn ? VPD_MAX_OPTIMAL_DAY : VPD_MAX_OPTIMAL_NIGHT;
    float minOpt = lampsAreOn ? VPD_MIN_OPTIMAL_DAY : VPD_MIN_OPTIMAL_NIGHT;

    bool kasIsDumpingMoisture = (fanExt1Speed > 0 || kasSmoothedHum > 75.0f || kasVpd > maxOpt || kasVpd < minOpt);
    bool indoorTooHumid = (indoorSmoothedHum > INDOOR_HUM_MAX_TARGET);
    
    float targetIndoorVpdMin = INDOOR_VPD_MIN_TARGET;
    if (learnedClimateBias < -0.3f) {
      targetIndoorVpdMin = 0.90f; 
    } else if (learnedClimateBias > 0.3f) {
      targetIndoorVpdMin = 0.75f; 
    }

    bool indoorVpdTooLow = (!isnan(indoorVpd) && indoorVpd < targetIndoorVpdMin); 
    bool outsideAirIsSignificantlyDrier = (outDp < (inDp - INDOOR_DEWPOINT_MARGIN));
    bool forecastBadWeather = (outdoorHumidity > OUTDOOR_MAX_HUMIDITY_FOR_VENT || forecastedTempSoon < 0.0f);

    HouseVentState desiredAdvice = HOUSE_CLOSED;
    String newReason = "";
    float projectedIndoorHum = indoorSmoothedHum + (humidityVelocity * 15.0f);

    if ((indoorTooHumid || indoorVpdTooLow || kasIsDumpingMoisture || projectedIndoorHum > INDOOR_PROJECTED_HUM_MAX) && outsideAirIsSignificantlyDrier && !forecastBadWeather) {
      desiredAdvice = HOUSE_VENTILATE;
      if (upcomingLightsOn && indoorVpdTooLow) {
        newReason = "Lamp-anticipatie actief: de kweeklampen gaan zo aan. De binnenbuffer wordt nu vast slim opgeschoond met frisse, droge buitenlucht.";
      } else if (indoorVpdTooLow) {
        newReason = "De binnenlucht wordt wat aan de klamme kant. Buitenlucht is droger, dus ventileren helpt de buffer fris te houden.";
      } else if (kasIsDumpingMoisture && indoorTooHumid) {
        newReason = "De kas voert vocht af en binnen is het wat vochtig. Buitenlucht is gunstig om nu even door te luchten.";
      } else {
        newReason = "Binnen is het wat vochtig aan het worden. Buitenlucht is helder en droger om de buffer te herstellen.";
      }
    } else {
      desiredAdvice = HOUSE_CLOSED;
      if (forecastBadWeather) {
        newReason = "Binnen en kas zijn in balans. Buiten wordt slecht weer verwacht, dus we houden alles lekker dicht.";
      } else if (!outsideAirIsSignificantlyDrier) {
        newReason = "Binnen en kas zijn in balans. Buitenlucht is momenteel te vochtig om extra te ventileren.";
      } else {
        newReason = "Het binnenklimaat is stabiel; de binnenbuffer heeft alle ruimte en capaciteit voor de kas.";
      }
    }

    unsigned long evalNow = millis();
    if (desiredAdvice != activeAdviceState) {
      if (!pendingEscalationCheck) {
        pendingEscalationCheck = true;
        escalationTimer = evalNow;
        baselineIndoorHum = indoorSmoothedHum;
        baselineKasVpd = kasVpd;
        logToSyslogAndSerialPrintf("[INDOOR ADVICE] Klimaatverschuiving of buffer-actie gedetecteerd. Start 15-minuten evaluatievenster.");
      } else if (evalNow - escalationTimer >= INDOOR_ESCALATION_TIMEOUT) { 
        bool worsening = (indoorSmoothedHum > baselineIndoorHum + 1.0f) || (kasVpd > baselineKasVpd + 0.15f);
        bool improving = (indoorSmoothedHum < baselineIndoorHum) || (kasVpd < baselineKasVpd);

        if (worsening) {
          String escMsg = "*MELDING:* De luchtvochtigheid binnenshuis blijft wat oplopen. Extra luchten wordt aangeraden.";
          logToSyslogAndSerialPrintf("[INDOOR ADVICE ESCALATION] Klimaat blijft escaleren na 15 minuten.");
          sendTelegramAlert(escMsg);
        } else if (improving) {
          String succMsg = "*GOED NIEUWS:* Binnenventilatie heeft de buffer weer netjes hersteld.";
          logToSyslogAndSerialPrintf("[INDOOR ADVICE SUCCESS] Ventileren effectief, klimaat stabiliseert.");
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

  applyLampLearning(lampsAreOn, kasSmoothedTemp);

  // Gebruik geleerde lamp-offset voor doeltemperatuur
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

  // Dynamische lamp-overgangsboost gekoppeld aan de geleerde stabilisatietijd
  if (isLampTransitionActive) {
    unsigned long transitionElapsed = now - lampTransitionStartTime;
    // Boostduur dynamisch afhankelijk van geleerde stabilisatietijd (max 20 min)
    unsigned long dynamicBoostDuration = min(learnedStabilizationTime * 60000UL, 1200000UL); 
    if (transitionElapsed < dynamicBoostDuration) {
      pidOutput += 10.0f; 
    } else {
      isLampTransitionActive = false; 
    }
  }

  int rawPidOutput = (int)pidOutput;

  // Interne ventilator met dynamische hysterese tegen klapperen
  bool internalFanNeeded = (moldRisk || kasSmoothedHum > 78.0f || rawPidOutput > 0.0f || isPreemptiveLampConditioningActive);
  static bool internalFanActiveState = false;
  
  if (internalFanNeeded) {
    internalFanActiveState = true;
  } else if (kasSmoothedHum < (75.0f - (learnedThermalInertia * 0.5f)) && rawPidOutput < -5.0f && !moldRisk) {
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
    finalIntSpeed += 25; 
    if (finalIntSpeed > targetIntSpeed) finalIntSpeed = targetIntSpeed;
  } else if (targetIntSpeed < finalIntSpeed) {
    finalIntSpeed -= 25; 
    if (finalIntSpeed < INT_FAN_MIN_PWM) finalIntSpeed = 0;
  }
  if (targetIntSpeed == 0 && finalIntSpeed < INT_FAN_MIN_PWM) finalIntSpeed = 0;

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

  // Slimme PID-afbouw gekoppeld aan learnedStabilizationTime (voorkomt abrupt stoppen of te lang blazen)
  int finalExt1Speed = fanExt1Speed;
  if (targetExt1Speed > finalExt1Speed) {
    finalExt1Speed = targetExt1Speed;
  } else if (targetExt1Speed < finalExt1Speed) {
    // Hoe langer de geleerde stabilisatietijd, hoe geleidelijker de ventilator afbouwt
    int stepDown = max(1, (int)(30 / (learnedStabilizationTime + 5)));
    finalExt1Speed -= stepDown;
    if (finalExt1Speed < EXT_FAN_MIN_PWM && finalExt1Speed > 0) finalExt1Speed = 0; 
    if (targetExt1Speed == 0 && finalExt1Speed < INT_FAN_MIN_PWM) finalExt1Speed = 0;
  }

  applySelfLearning(currentTemp, targetTemp, finalExt1Speed);

  fanIntSpeed = finalIntSpeed;
  fanExt1Speed = finalExt1Speed;

  ledcWrite(PIN_FAN_INT_PWM, fanIntSpeed);
  ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
}

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