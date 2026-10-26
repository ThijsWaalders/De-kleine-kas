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

// Externe vlag uit ClimateLogic of Config
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
const int REQUIRED_PERSISTENT_CHECKS = 3; // anti hunt even sneller laten werken om effect systeem te testen. // was 5; 

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

  bool outdoorCanDehumidifyHome = (homeOutdoorDewPoint < homeIndoorDewPoint);
  bool indoorNeedsVentilation   = (inHum > HUM_MOLD_THRESHOLD) || (indoorVpd > VPD_MAX_OPTIMAL) || (indoorVpd < VPD_MIN_OPTIMAL);

  homeVentActive = (indoorNeedsVentilation && outdoorCanDehumidifyHome && tempSafeForHome);
}

void evaluateKasVentilationDecision(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;

  evaluateHomeVentilationDecision(inTemp, inHum, outTemp, outHum);

  // --- FASE 0: HARDE RANDVOORWAARDEN ---
  if (isKasSleeping) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "💤 De kleine kas slaapt...";
    vpdRedZoneActive = false;
    return;
  }

  if (!isSystemReady()) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "⏳ Systeem start op...";
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

  // --- ALS BEIDE TRAYS IN KIEMFASE STAAN (GEEN ENKELE TRAY IN GROEI/WM) ---
  if (!anyTrayInGrowMode) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "🌱 De kleine kas is gesloten; Kiemfase actief (Lampen/fans uit. Alleen tray-bewaking en woning/buiten-data actief.)";
    
    // Alleen ingrijpen als een kiem-tray buiten syncoop raakt (te heet/koud voor de kiem)
    if ((traySensor1Connected && tray1Mode == 0 && (tray1TooHot || tray1TooCold)) ||
        (traySensor2Connected && tray2Mode == 0 && (tray2TooHot || tray2TooCold))) {
      kasAdviceReason = "🌱⚠️ *KIEMFASE WAARSCHUWING;* Tray temperatuur buiten kiemgrenzen (17.5°C - 23.5°C)!";
    }
    
    // Sla de rest van de kas-beslisboom over omdat de kas zelf in rust is
    return;
  }

  // --- VANAF HIER DRAAIT DE VOLLEDIGE KAS-BESLISBOOM (wanneer minstens 1 tray in groei/wm mode staat) ---
  kasAdviceReason = "🔄 De kleine kas is gesloten / in balans.";

  bool isTooWarm      = (inTemp > GREENHOUSE_MAX_TEMP);
  bool isKritischHeet = (inTemp >= GREENHOUSE_CRASH_TEMP); 
  bool isTooWet       = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
  bool isTooDry       = (kasVpd > VPD_MAX_OPTIMAL); 
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

  bool indoorCanHelpDehumidify = (!isnan(indoorVpd) && indoorVpd > kasVpd && indoorVpd < VPD_MAX_OPTIMAL);
  bool indoorCanHelpHumidify   = (!isnan(indoorVpd) && indoorVpd < kasVpd && indoorVpd > VPD_MIN_OPTIMAL);

  // ============================================
  // FASE 1 t/m 5 van de beslisboom
  // ============================================
  if (isClimateCrash || isKritischHeet) {
    rawDecision = GREENHOUSE_VENTILATE;
    kasAdviceReason = "🌬️🚨 *NOODTOESTAND;* De kleine kas is kritisch heet (" + String(inTemp, 1) + "°C)! Ventilatie op MAX!";
  }
  else if (anyTrayActionNeeded) {
    rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "🔄🔥 *TRAY BESCHERMING;* Tray & woning temperatuur te hoog (" + String(offendingTrayTemp, 1) + "°C)!";
  }
  else if (vpdRedZoneActive && (currentMillis - vpdRedZoneStartTime > VPD_ESCALATION_TIMEOUT)) {
    if (isTooDry) {
      rawDecision = indoorCanHelpHumidify ? GREENHOUSE_VENTILATE : GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "⚠️ *ADAPTIEVE ESCALATIE;* Kas te lang te droog (" + String(kasVpd, 2) + " kPa)!";
    } else {
      rawDecision = indoorCanHelpDehumidify ? GREENHOUSE_VENTILATE : GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "⚠️ *ADAPTIEVE ESCALATIE;* Vocht/klamheid duurt te lang (" + String(kasVpd, 2) + " kPa)!";
    }
  }
  else if (isTooWarm) {
    if (indoorCanHelpDehumidify || inTemp > (GREENHOUSE_MAX_TEMP + 1.0f)) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "🌬️🌡️ Te warm (" + String(inTemp, 1) + "°C); Externe ventilatie actief.";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🔄🌡️ De kleine kas is gesloten; te warm & woninglucht helpt niet.";
    }
  }
  else if (isTooWet || condensRisk) {
    if (indoorCanHelpDehumidify) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "🌬️💧 De kleine kas is te klam; woninglucht wordt ingezet.";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🔄🌧️ De kleine kas is gesloten; te klam & woninglucht helpt niet.";
    }
  }
  else if (isTooDry) {
    if (indoorCanHelpHumidify && (kasVpd - VPD_MAX_OPTIMAL > 0.05f)) {
      rawDecision = GREENHOUSE_VENTILATE;
      kasAdviceReason = "🌬️🔥 De kleine kas ventileert; te droog & woninglucht biedt vochtcorrectie.";
    } else {
      rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🔄💡 De kleine kas is gesloten; te droog & woninglucht helpt niet.";
    }
  }
  else {
    rawDecision = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "🔄 De kleine kas is gesloten; Klimaat is optimaal.";
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
