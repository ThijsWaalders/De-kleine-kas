/**
 * @file ClimateLogic.cpp
 * @brief Zuivere klimaatlogica en advies-evaluatie (zonder hardware-aansturing).
 * 
 * Beheert de 3 gescheiden domeinen:
 * 1. KAS (Microklimaat & Teelt - SHT31)
 * 2. WONING (Bufferruimte & Binnenklimaat - BME280)
 * 3. BUITEN (Omgevingslucht & Weer-API)
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "IndoorSensors.h"
#include "KasSensors.h"
#include "Logger.h"
#include "LedManager.h"

extern LedManager statusLed;

// Voorkomt directe valse start-alarmen
static unsigned long lastAlertTime = millis(); 

bool isSystemReady();
bool isClimateCrash = false; 

bool isSystemReady() {
  // Als lampen aan staan (lux > 1.0), na 5 sec direct actief!
  // Als lampen uit staan, wacht netjes de volledige opstartvertraging af.
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

void evaluateClimateState(float tempC, float hum) {
  if (isnan(tempC) || isnan(hum)) return;

  if (!isSystemReady()) {
    moldRisk = false;
    moldReasonText = "⏳ Systeem kalibreert / leert...";
    vpdStatusText = " `(⏳ Opstarten)`";
    dpMarginStatusText = " `(⏳ Kalibreren)`";
    return;
  }

  // --- DOMEIN 1: KAS METINGEN & BEREKENINGEN ---
  kasSmoothedTemp = tempC;
  kasSmoothedHum  = hum;
  displayedTemp   = tempC; 

  kasDewPoint = calcDewPoint(kasSmoothedTemp, kasSmoothedHum);
  kasVpd = calcVPD(kasSmoothedTemp, kasSmoothedHum);
  kasDpMargin = kasSmoothedTemp - kasDewPoint;

  checkClimateCrash(kasSmoothedTemp);

  if (isLampTransitionActive) {
    if (millis() - lampTransitionStartTime > 180000 && abs(humidityVelocity) < 0.3) {
      isLampTransitionActive = false;
      logToSyslogAndSerial("[PREDICTIVE] Kas is adaptief gestabiliseerd na lamp-transitie.");
    }
    else if (millis() - lampTransitionStartTime > 1800000) {
      isLampTransitionActive = false;
    }
  }

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

  // LED Status & Langdurige Telegram Alarm Cooldown (Pas na 20 minuten aanhoudende overschrijding)
  static unsigned long problemStartTime = 0; 

  if (isClimateCrash || tempTooHigh) {
    statusLed.setColor(255, 0, 0); 
    problemStartTime = 0;
  } 
  else if (vpdTooDry || vpdTooWet || condensRisk) {
    statusLed.setColor(255, 100, 0); 
    
    // Houd bij hoelang de kas al onafgebroken in deze afwijkende zone zit
    if (problemStartTime == 0) problemStartTime = millis();

    // Stuur PAS na 20 minuten een krachtig capaciteitsalarm met actie-instructie voor jou!
    if (millis() - problemStartTime > 1200000) {
      static unsigned long lastAlertTime = 0;
      if ((millis() - lastAlertTime > 7200000 || lastAlertTime == 0) && ENABLE_TELEGRAM && isConnected) {
        lastAlertTime = millis();
        
        String capaciteitsBericht = "🔥 *CAPACITEITSALARM DE KLEINE KAS:*\n" + moldReasonText + 
                                    "\n\n⚙️ *Waarom:* Ventilatoren draaien op volle capaciteit, maar de woningbuffer kan de warmte/vochtbalans niet meer volledig corrigeren." +
                                    "\n👤 *Wat JIJ moet doen:* Zet een raam in de woning open om de luchtbuffer te verversen met frisse, drogere buitenlucht!";
        sendTelegramAlert(capaciteitsBericht);
      }
    }
  }
  else {
    statusLed.setColor(0, 0, 0); 
    problemStartTime = 0; // Reset de timer direct zodra het klimaat weer stabiel is!
  }
  // // LED Status & Langdurige Telegram Alarm Cooldown (Pas na 20 minuten aanhoudende overschrijding)
  // static unsigned long problemStartTime = 0; // Hier gedefinieerd zodat de hele functie erbij kan!

  // if (isClimateCrash || tempTooHigh) {
  //   statusLed.setColor(255, 0, 0); 
  //   problemStartTime = 0;
  // } 
  // else if (vpdTooDry || vpdTooWet || condensRisk) {
  //   statusLed.setColor(255, 100, 0); 
    
  //   // Houd bij hoelang de kas al onafgebroken in deze afwijkende zone zit
  //   if (problemStartTime == 0) problemStartTime = millis();

  //   // Stuur PAS een langdurig alarm als het probleem al MINIMAAL 20 minuten duurt (1200000 ms)
  //   if (millis() - problemStartTime > 1200000) {
  //     static unsigned long lastAlertTime = 0;
  //     if ((millis() - lastAlertTime > 7200000 || lastAlertTime == 0) && ENABLE_TELEGRAM && isConnected) {
  //       lastAlertTime = millis();
  //       sendTelegramAlert("🔥 *LANGDURIG ALARM DE KLEINE KAS:* " + moldReasonText + "\nSysteem probeert al langdurig te corrigeren, maar kan het klimaat onvoldoende stabiliseren.");
  //     }
  //   }
  // }
  // else {
  //   statusLed.setColor(0, 0, 0); 
  //   problemStartTime = 0; // Reset de timer direct zodra het klimaat weer stabiel is!
  // }
  // // LED Status & Langdurige Telegram Alarm Cooldown (2 uur)
  // if (isClimateCrash || tempTooHigh) {
  //   statusLed.setColor(255, 0, 0); 
  // } 
  // else if (vpdTooDry || vpdTooWet || condensRisk) {
  //   statusLed.setColor(255, 100, 0); 
  //   static unsigned long lastAlertTime = 0;
  //   if ((millis() - lastAlertTime > 7200000 || lastAlertTime == 0) && ENABLE_TELEGRAM && isConnected) {
  //     lastAlertTime = millis();
  //     sendTelegramAlert("🔥 *LANGDURIG ALARM DE KLEINE KAS:* " + moldReasonText + "\nSysteem kan het klimaat onvoldoende stabiliseren.");
  //   }
  // }
  // else {
  //   statusLed.setColor(0, 0, 0); 
  // }

  if (isClimateCrash) vpdStatusText = " `(🚨 NOODSTOP)`";
  else if (vpdTooWet) vpdStatusText = " `(⚠️ Te klam)`";
  else if (vpdTooDry) vpdStatusText = " `(🔥 Te droog)`";
  else vpdStatusText = " `(✅ Optimaal)`";

  if (condensRisk) dpMarginStatusText = " `(⚠️ Risico)`";
  else dpMarginStatusText = " `(✅ Veilig)`";

  // --- DOMEIN 2: WONING METINGEN & BEREKENINGEN ---
  if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum)) {
    indoorDewPoint = calcDewPoint(indoorSmoothedTemp, indoorSmoothedHum);
    indoorVpd = calcVPD(indoorSmoothedTemp, indoorSmoothedHum);
    indoorDpMargin = indoorSmoothedTemp - indoorDewPoint;
  }

  // --- DOMEIN 3: BUITEN METINGEN & BEREKENINGEN ---
  if (!isnan(outdoorTemp) && !isnan(outdoorHumidity)) {
    // BRON-CHECK: Voorkom wiskundige uitschieters bij wegvallen API/netwerk of 0% LV
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
//   // --- DOMEIN 3: BUITEN METINGEN & BEREKENINGEN ---
//   if (!isnan(outdoorTemp) && !isnan(outdoorHumidity)) {
//     outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
//     outdoorVpd = calcVPD(outdoorTemp, outdoorHumidity);
//     outdoorDpMargin = outdoorTemp - outdoorDewPoint; 
//   }
// }

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

/**
 * @brief Berekent het ventilatie-advies voor de RUIMTE / WONING (voor de bewoner).
 * Evalueert fysieke vochtpotentie (dauwpunt) van de buitenlucht in plaats van blinde kap op %LV.
 */
void updateHomeVentilationAdvice(float inTemp, float inHum, float outTemp, float outHum) {
  if (isnan(inHum) || isnan(outHum) || isnan(inTemp) || isnan(outTemp)) return;

  // 1. Extreme veiligheidsgrenzen voor de woning (voorkom vorstschade of extreme hitte)
  bool tempSafeForHome = (outTemp > 1.0f && outTemp < 35.0f); 

  // 2. Beoordeel absolute vocht-potentie op basis van Dauwpunt:
  // Koudere buitenlucht (bijv. 7°C / 92% LV) heeft een erg LAGE dauwpuntwaarde (~5.7°C).
  // Zodra die lucht de woning in stroomt en opwarmt naar 20°C, daalt de relatieve vochtigheid enorm!
  float homeOutdoorDewPoint = calcDewPoint(outTemp, outHum);
  float homeIndoorDewPoint  = calcDewPoint(inTemp, inHum);

  // Buitenlucht is ontvochtigend als het buiten-dauwpunt LAGER is dan het binnen-dauwpunt
  bool outdoorCanDehumidifyHome = (homeOutdoorDewPoint < homeIndoorDewPoint);

  // Woning heeft ventilatie nodig als de lucht te klam is (inHum > 55%) of muf/droog
  bool indoorNeedsVentilation = (inHum > 55.0f) || (indoorVpd > VPD_MAX_OPTIMAL) || (indoorVpd < VPD_MIN_OPTIMAL);

  // Woning-ventilatie is actief als het nuttig is én buiten fysieke potentie heeft én temperatuur veilig is
  homeVentActive = (indoorNeedsVentilation && outdoorCanDehumidifyHome && tempSafeForHome);
}

/**
 * @brief Berekent het ventilator-advies voor de KAS.
 * Bepaalt of de kas gesloten blijft (Interne circulatie) of gekoeld/gecorrigeerd moet worden met Woninglucht.
 */
void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;
  
  // Update eerst het advies voor het woningdomein
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
  
  // Woninglucht is bruikbaar als de woning-VPD gunstiger is dan de kas-VPD
  bool indoorCanHelpDehumidify = (!isnan(indoorVpd) && indoorVpd > kasVpd);
  bool indoorCanHelpHumidify   = (!isnan(indoorVpd) && indoorVpd < kasVpd);

  // 1. KRITIEKE TEMPERATUREN & NOODSTOFFEN (Prioriteit 1)
  if (isClimateCrash || isKritischHeet || isTooWarm) {
    rawNewAdvice = GREENHOUSE_VENTILATE;
    kasAdviceReason = "🌡️ Te warm (" + String(inTemp, 1) + "°C). Externe ventilatie actief met woninglucht.";
  }
  // 2. KAS TE KLAM / VOCHTIG (VPD te laag)
  else if (isTooWet || condensRisk) {
    if (indoorCanHelpDehumidify) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = "💧 Kas te klam; woninglucht inzetten voor ontvochtiging.";
    } else {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🌧️ Kas te klam, maar woninglucht helpt niet. Kas blijft dicht (Interne circulatie).";
    }
  }
  // 3. KAS TE DROOG (VPD te hoog - Bijv. door lampwarmte)
  else if (isTooDry || lampsAreOn) {
    if (indoorCanHelpHumidify) {
      rawNewAdvice = GREENHOUSE_VENTILATE;
      kasAdviceReason = lampsAreOn 
        ? "💡 Lampen aan & kas te droog; woninglucht wordt ingezet om warmte/VPD te balanceren."
        : "🔥 Kas te droog; woninglucht inzetten voor correctie.";
    } else {
      rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
      kasAdviceReason = "🔥 Kas te droog, maar woninglucht helpt niet. Kas blijft dicht (Interne circulatie).";
    }
  }
  // 4. ALLES OPTIMAAL
  else {
    rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
    kasAdviceReason = "🔄 Interne circulatie actief (standaard kas-balans).";
  }

  // --- PERSISTENTIE & DEBOUNCE LOGICA ---
  static KasVentState candidateAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
  static int persistenceCounter = 0;
  const int REQUIRED_PERSISTENT_CHECKS = 3;

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

void updateHighLow(float currentTemp) {
  if (isnan(currentTemp)) return;
  (void)currentTemp;
}

void updateHumidityHighLow(float kasHum, float indoorHum, float outHum) {
  (void)kasHum; (void)indoorHum; (void)outHum;
}

void handleTestModeTimeout() {
  if (!isTestModeActive) return;
  if (millis() - testModeStartTime >= currentTestDuration) {
    isTestModeActive = false;
    logToSyslogAndSerial("[TEST] Testmodus automatisch afgelopen.");
  }
}
