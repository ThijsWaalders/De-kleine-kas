/**
 * @file DecisionEngine.cpp
 * @brief Implementatie van het besluitvormingsproces, anti-hunt filtering en adaptieve escalatie inclusief Tray-bescherming.
 */

#include "DecisionEngine.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include "Telegram.h"

// Voor beslisboom
bool homeVentActive = false;

// Externe variabelen uit ClimateLogic of Config
extern bool isClimateCrash;
extern float traySensor1Temp;
extern float traySensor2Temp;
extern bool traySensor1Connected;
extern bool traySensor2Connected;
extern int tray1Mode; // 0 = Kiemfase, 1 = Groeifase, 2 = Warmtemat-modus
extern int tray2Mode;

// Anti-hunt en stabilisatie variabelen
static unsigned long fanStateChangeTime = 0;
static KasVentState lastAppliedKasAdvice = KAS_OFF;
static int persistentCheckCounter = 0;
static KasVentState candidateDecision = GREENHOUSE_CIRCULATE_INTERNAL;
const int REQUIRED_PERSISTENT_CHECKS = 3; // Anti-hunt filter

static unsigned long vpdRedZoneStartTime = 0;
bool vpdRedZoneActive = false;
const unsigned long VPD_ESCALATION_TIMEOUT = 600000; // 10 minuten

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

void evaluateHomeVentilationDecision(float inTemp, float inHum, float outTemp, float outHum) {
  if (isnan(inHum) || isnan(outHum) || isnan(inTemp) || isnan(outTemp)) return;

  bool tempSafeForHome = (outTemp > 1.0f && outTemp < 35.0f); 
  float homeOutdoorDewPoint = calcDewPoint(outTemp, outHum);
  float homeIndoorDewPoint  = calcDewPoint(inTemp, inHum);

  // Gebruik de centrale INDOOR_DEWPOINT_MARGIN (1.5°C) i.p.v. hardgecodeerde marge
  bool outdoorCanDehumidifyHome = (homeOutdoorDewPoint < (homeIndoorDewPoint - INDOOR_DEWPOINT_MARGIN));
  
  // Gekoppeld aan de centrale drempels uit Config.h (INDOOR_HUM_MAX_TARGET = 70%, INDOOR_VPD_MIN_TARGET = 0.72f)
  bool indoorNeedsVentilation = (inHum > INDOOR_HUM_MAX_TARGET) || (!isnan(indoorVpd) && indoorVpd < INDOOR_VPD_MIN_TARGET);

  homeVentActive = (indoorNeedsVentilation && outdoorCanDehumidifyHome && tempSafeForHome);
}

void evaluateKasVentilationDecision(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;

  evaluateHomeVentilationDecision(inTemp, inHum, outTemp, outHum);

  // --- FASE 0: HARDE RANDVOORWAARDEN ---
  if (isKasSleeping) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "😴 De kleine kas slaapt (nachtrust)...";
    vpdRedZoneActive = false;
    return;
  }

  if (!isSystemReady()) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "⏳ Systeem opstarten en kalibreren...";
    vpdRedZoneActive = false;
    return;
  }

  // --- TRAY VEILIGHEIDSCHECK & MODUS BEPALING ---
  bool tray1TooHot = false, tray1TooCold = false;
  bool tray2TooHot = false, tray2TooCold = false;

  // Check Tray 1
  if (traySensor1Connected && traySensor1Temp > -100.0f) {
    float maxLimit = (tray1Mode == 0) ? 23.5f : ((tray1Mode == 1) ? 25.0f : 29.0f);
    float minLimit = (tray1Mode == 0) ? 17.5f : ((tray1Mode == 1) ? 15.0f : 16.0f);
    if (traySensor1Temp > maxLimit) tray1TooHot = true;
    if (traySensor1Temp < minLimit) tray1TooCold = true;
  }

  // Check Tray 2
  if (traySensor2Connected && traySensor2Temp > -100.0f) {
    float maxLimit = (tray2Mode == 0) ? 23.5f : ((tray2Mode == 1) ? 25.0f : 29.0f);
    float minLimit = (tray2Mode == 0) ? 17.5f : ((tray2Mode == 1) ? 15.0f : 16.0f);
    if (traySensor2Temp > maxLimit) tray2TooHot = true;
    if (traySensor2Temp < minLimit) tray2TooCold = true;
  }

  // Bepaal of minstens één tray in de groeimodus / warmtemat-modus staat (dus NIET kiemmodus 0)
  bool anyTrayInGrowMode = false;
  if (traySensor1Connected && tray1Mode != 0) anyTrayInGrowMode = true;
  if (traySensor2Connected && tray2Mode != 0) anyTrayInGrowMode = true;

  // Alleen ingrijpen op beslisboom per tray als ze NIET in kiemmodus staan
  bool tray1ActionRequired = (tray1TooHot && tray1Mode != 0);
  bool tray2ActionRequired = (tray2TooHot && tray2Mode != 0);
  bool anyTrayActionNeeded = (tray1ActionRequired || tray2ActionRequired);
  float offendingTrayTemp = tray1ActionRequired ? traySensor1Temp : traySensor2Temp;

  KasVentState rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;

  // --- ALS BEIDE TRAYS IN KIEMFASE STAAN ---
  if (!anyTrayInGrowMode) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "Gesloten: Kiemfase actief (achtergrondregeling)";
    
    if ((traySensor1Connected && tray1Mode == 0 && (tray1TooHot || tray1TooCold)) ||
        (traySensor2Connected && tray2Mode == 0 && (tray2TooHot || tray2TooCold))) {
      kasAdviceReason = "Waarschuwing: Tray-temperatuur buiten kiemgrenzen";
    }
    return;
  }

  // --- DYNAMISCHE & LAMP-AWARE VPD DREMPELS ---
  float maxOptimalVpd = lampsAreOn ? VPD_MAX_OPTIMAL_DAY : VPD_MAX_OPTIMAL_NIGHT;
  float minOptimalVpd = lampsAreOn ? VPD_MIN_OPTIMAL_DAY : VPD_MIN_OPTIMAL_NIGHT;

  bool isTooWarm      = (inTemp > GREENHOUSE_MAX_TEMP);
  bool isKritischHeet = (inTemp >= GREENHOUSE_CRASH_TEMP); 
  bool isTooWet       = (kasVpd < minOptimalVpd) || (kasSmoothedHum >= HUM_MOLD_THRESHOLD);
  bool isTooDry       = (kasVpd > maxOptimalVpd); 
  bool condensRisk    = (kasDpMargin < DP_MARGIN_MIN);

  bool isVpdInRedZone = (isTooWet || isTooDry || condensRisk || anyTrayActionNeeded);
  unsigned long currentMillis = millis();

  if (isVpdInRedZone) {
    if (!vpdRedZoneActive) {
      vpdRedZoneActive = true;
      vpdRedZoneStartTime = currentMillis;
    }
  } else {
    vpdRedZoneActive = false;
    vpdRedZoneStartTime = 0;
  }

  bool indoorCanHelpDehumidify = (!isnan(indoorVpd) && indoorVpd > kasVpd && indoorVpd < maxOptimalVpd);
  bool indoorCanHelpHumidify   = (!isnan(indoorVpd) && indoorVpd < kasVpd && indoorVpd > minOptimalVpd);

  // ============================================
  // BESLISBOOM (PROFESSIONEEL & GRAFANA-FRIENDLY)
  // ============================================
  if (isClimateCrash || isKritischHeet) {
    rawDecision = GREENHOUSE_VENTILATE;
    kasAdviceReason = "Noodtoestand: Kritieke temperatuur (" + String(inTemp, 1) + "C)";
  }
  else if (anyTrayActionNeeded) {
    rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "Tray-bescherming: Temperatuur te hoog (" + String(offendingTrayTemp, 1) + "C)";
  }
  else if (vpdRedZoneActive && (currentMillis - vpdRedZoneStartTime > VPD_ESCALATION_TIMEOUT)) {
    if (isTooDry) {
      rawDecision = indoorCanHelpHumidify ? GREENHOUSE_VENTILATE : GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "Adaptieve escalatie: Langdurige droogte (" + String(kasVpd, 2) + " kPa)";
    } else {
      rawDecision = indoorCanHelpDehumidify ? GREENHOUSE_VENTILATE : GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "Adaptieve escalatie: Langdurige klamheid (" + String(kasVpd, 2) + " kPa)";
    }
  }
  else if (isTooWarm) {
    if (indoorCanHelpDehumidify || inTemp > (GREENHOUSE_MAX_TEMP + 1.0f)) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "Koeling actief: Externe ventilatie (" + String(inTemp, 1) + "C)";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "Kas gesloten: Interne warmte-afvoer";
    }
  }
  else if (isTooWet || condensRisk) {
    if (indoorCanHelpDehumidify) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "Vocht-uitwisseling: Woningbuffer ingezet";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "Kas gesloten: Interne vochtstabilisatie";
    }
  }
  else if (isTooDry) {
    if (indoorCanHelpHumidify && (kasVpd - maxOptimalVpd > 0.05f)) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "Droogte-correctie: Woninglucht ingezet";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "Kas gesloten: Optimale luchtcirculatie";
    }
  }
  else {
    rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "Gesloten: Klimaat optimaal";
  }

  // Anti-hunting filter toepassen
  if (rawDecision != lastAppliedKasAdvice) {
    if (rawDecision == candidateDecision) {
      persistentCheckCounter++;
      if (persistentCheckCounter >= REQUIRED_PERSISTENT_CHECKS) {
        kasAdvice = applyAntiHuntFilter(rawDecision);
        persistentCheckCounter = 0;
      }
    } else {
      candidateDecision = rawDecision;
      persistentCheckCounter = 1;
    }
  } else {
    persistentCheckCounter = 0;
    kasAdvice = applyAntiHuntFilter(rawDecision);
  }

  if (kasAdvice != previousKasAdvice) {
    previousKasAdvice = kasAdvice;
    logToSyslogAndSerialPrintf("[DECISION] Nieuwe beslissing: %s", kasAdviceReason.c_str());
  }
}