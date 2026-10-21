/**
 * @file DecisionEngine.cpp
 * @brief Implementatie van het besluitvormingsproces, anti-hunt filtering en adaptieve escalatie voor de klimaatregeling.
 */

#include "DecisionEngine.h"
#include "ClimateLogic.h"
#include "Logger.h"

// Voor beslisboom
bool homeVentActive = false;

// Externe vlag uit ClimateLogic of Config
extern bool isClimateCrash;

// Anti-hunt en stabilisatie variabelen
static unsigned long fanStateChangeTime = 0;
static KasVentState lastAppliedKasAdvice = KAS_OFF;
static int persistentCheckCounter = 0;
static KasVentState candidateDecision = GREENHOUSE_CIRCULATE_INTERNAL;
const int REQUIRED_PERSISTENT_CHECKS = 5; // 5 cycli vertraging tegen het "hoppa"-effect

// Adaptieve timers voor langdurige afwijkingen (zelflerende escalatie)
static unsigned long vpdRedZoneStartTime = 0;
static bool vpdRedZoneActive = false;
const unsigned long VPD_ESCALATION_TIMEOUT = 600000; // 10 minuten in de roodwaarde = geforceerde escalatie

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

  bool outdoorCanDehumidifyHome = (homeOutdoorDewPoint < homeIndoorDewPoint);
  bool indoorNeedsVentilation   = (inHum > HUM_MOLD_THRESHOLD) || (indoorVpd > VPD_MAX_OPTIMAL) || (indoorVpd < VPD_MIN_OPTIMAL);

  homeVentActive = (indoorNeedsVentilation && outdoorCanDehumidifyHome && tempSafeForHome);
}

void evaluateKasVentilationDecision(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;

  // Update eerst het advies voor de woning
  evaluateHomeVentilationDecision(inTemp, inHum, outTemp, outHum);

  // --- FASE 0: HARDE RANDVOORWAARDEN ---
  if (isKasSleeping) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "💤 De kleine kas is in slaapstand. Klimaatregeling gepauzeerd.";
    vpdRedZoneActive = false;
    return;
  }

  if (!isSystemReady()) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "⏳ Systeem is aan het opstarten...";
    vpdRedZoneActive = false;
    return;
  }

  KasVentState rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
  kasAdviceReason = "🔄 Interne circulatie actief (standaard kas-balans).";

  // Fysieke condities binnen de kas
  bool isTooWarm      = (inTemp > GREENHOUSE_MAX_TEMP);
  bool isKritischHeet = (inTemp >= GREENHOUSE_CRASH_TEMP); 
  bool isTooWet       = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
  bool isTooDry       = (kasVpd > VPD_MAX_OPTIMAL); 
  bool condensRisk    = (kasDpMargin < DP_MARGIN_MIN);

  // Controleer of de VPD in de gevarenzone (rood) hangt
  bool isVpdInRedZone = (isTooWet || isTooDry || condensRisk);
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

  // Fysieke geschiktheid van de woninglucht als buffer
  bool indoorCanHelpDehumidify = (!isnan(indoorVpd) && indoorVpd > kasVpd && indoorVpd < VPD_MAX_OPTIMAL);
  bool indoorCanHelpHumidify   = (!isnan(indoorVpd) && indoorVpd < kasVpd && indoorVpd > VPD_MIN_OPTIMAL);

  // --- FASE 1: NOODTOESTAND & HEAT CRASH ---
  if (isClimateCrash || isKritischHeet) {
    rawDecision = GREENHOUSE_VENTILATE;
    kasAdviceReason = "🚨 NOODTOESTAND: Kas te heet (" + String(inTemp, 1) + "°C). Maximale externe ventilatie ingezet!";
  }
  // --- ADAPTIEVE ESCALATIE BIJ LANGDURIGE VPD-OVERSCHRIJVING ---
  else if (vpdRedZoneActive && (currentMillis - vpdRedZoneStartTime > VPD_ESCALATION_TIMEOUT)) {
    if (isTooDry) {
      rawDecision = indoorCanHelpHumidify ? GREENHOUSE_VENTILATE : GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "⚠️ *ADAPTIEVE ESCALATIE:* Kas te lang te droog (" + String(kasVpd, 2) + " kPa). Dwingende correctie actief!";
    } else {
      rawDecision = indoorCanHelpDehumidify ? GREENHOUSE_VENTILATE : GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "⚠️ *ADAPTIEVE ESCALATIE:* Vocht/klamheid duurt te lang (" + String(kasVpd, 2) + " kPa). Dwingende correctie actief!";
    }
  }
  // --- FASE 2: WARMTE OVERSCHRIJDING ---
  else if (isTooWarm) {
    if (indoorCanHelpDehumidify || inTemp > (GREENHOUSE_MAX_TEMP + 1.0f)) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "🌡️ Te warm (" + String(inTemp, 1) + "°C). Externe ventilatie actief met woninglucht.";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🌡️ Te warm, maar woninglucht helpt niet. Interne circulatie gekozen.";
    }
  }
  // --- FASE 3: VOCHT / SCHIMMELRISICO (TE KLAM) ---
  else if (isTooWet || condensRisk) {
    if (indoorCanHelpDehumidify) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "💧 Kas te klam (VPD " + String(kasVpd, 2) + "); woninglucht wordt ingezet voor ontvochtiging.";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🌧️ Kas te klam, maar woninglucht helpt niet. Kas blijft dicht op interne circulatie.";
    }
  }
  // --- FASE 4: UITDROGINGSRISICO / LAMPEN AAN ---
  else if (isTooDry) {
    if (indoorCanHelpHumidify && (kasVpd - VPD_MAX_OPTIMAL > 0.05f)) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "🔥 Kas te droog; woninglucht biedt gecontroleerde vochtcorrectie.";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "💡 Lampen aan / lichte droogte; interne circulatie houdt kiemgroenten veilig.";
    }
  }
  // // --- FASE 4: UITDROGINGSRISICO / LAMPEN AAN (TE DROOG) ---
  // else if (isTooDry || lampsAreOn) {
  //   if (indoorCanHelpHumidify) {
  //     rawDecision = GREENHOUSE_VENTILATE;
  //     kasAdviceReason = lampsAreOn 
  //       ? "💡 Lampen aan & kas te droog; woninglucht wordt direct ingezet om vocht toe te voegen."
  //       : "🔥 Kas te droog; woninglucht biedt direct vochtcorrectie.";
  //   } else {
  //     rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
  //     kasAdviceReason = lampsAreOn 
  //       ? "💡 Lampen aan & kas te droog, maar woninglucht helpt niet. Interne circulatie handhaaft het microklimaat."
  //       : "🔥 Kas te droog, maar woninglucht helpt niet. Kas blijft dicht op interne circulatie.";
  //   }
  // }
  // --- FASE 5: NORMAAL OPTIMAAL KLIMAAT ---
  else {
    rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "🔄 Interne circulatie actief (standaard kas-balans).";
  }

  // --- ANTI-HUNTING & STABILISATIE FILTER ---
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
