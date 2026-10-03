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
#include "LedManager.h"

extern LedManager statusLed;

// =========================================================================
// 1. VASTE CORRECTIES & KALIBRATIE
// =========================================================================
float kasTempOffset = 0.0; 
float kasHumOffset = 0.0;  

// Globale vlag voor de noodtoestand (automatisch herstel ingebouwd)
bool isClimateCrash = false; 

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
KasVentState lastAppliedKasAdvice = KAS_OFF;

// =========================================================================
// 4. HULPFUNCTIES: BUFFERING & GEMIDDELDES
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
// 5. KLIMAAT CRASH BEWAKING (Noodtoestand met automatische herstel-check)
// =========================================================================
void checkClimateCrash(float inTemp) {
  bool criticalTempHigh = (inTemp >= 30.0); // Gevaarlijk heet voor planten en hardware
  bool safeTempRecovery = (inTemp <= 27.0); // Veilige marge waarna het systeem zelfstandig herstelt

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

// =========================================================================
// 6. HOOFD KLIMAAT- & SCHIMMELDIAGNOSE (Lux-bewust & Directe Alarmering)
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

  checkClimateCrash(avgTemp);

  bool vpdTooWet = (kasVpd < VPD_MIN_OPTIMAL);
  bool vpdTooDry = (kasVpd > VPD_MAX_OPTIMAL);
  bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);
  bool humHigh = (avgHum >= HUM_MOLD_THRESHOLD);
  bool tempTooLow = (avgTemp < HEAT_MAT_TEMP_LOW);
  bool tempTooHigh = (avgTemp > GREENHOUSE_MAX_TEMP);
  
  // Bepaal of lampen aan staan via de lux-sensor
  bool lampsAreOn = (currentLuxValue > 1.0);

  // Evaluatie risico's met slim onderscheid (Lampen vs Omgeving)
  if (isClimateCrash) {
    moldRisk = true;
    moldReasonText = "🚨 NOODTOESTAND: Kas uit de hand gelopen (" + String(avgTemp, 1) + "°C)!";
  }
  else if (vpdTooWet) {
    moldRisk = true;
    moldReasonText = "⚠️ SCHIMMELRISICO: Kas te klam (VPD " + String(kasVpd, 2) + " < " + String(VPD_MIN_OPTIMAL, 2) + " kPa)";
  }
  else if (vpdTooDry) {
    moldRisk = false; 
    String cause = lampsAreOn ? "door brandende kweeklampen" : "door omgevingswarmte/droogte";
    moldReasonText = "⚠️ UITDROGINGSRISICO: Kas te droog " + cause + " (VPD " + String(kasVpd, 2) + " kPa)";
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
    String cause = lampsAreOn ? "door kweeklampen" : "door externe warmte";
    moldReasonText = "⚠️ WAARSCHUWING: Kiemtemperatuur te hoog " + cause + " (" + String(avgTemp, 1) + "°C)";
  }
  else {
    moldRisk = false;
    moldReasonText = "✅ Ideaal kiemklimaat";
  }

  // =========================================================================
  // HARDING VAN DE ALARMEN (LED & Telegram toevoeging)
  // =========================================================================
  if (isClimateCrash || tempTooHigh) {
    statusLed.setColor(255, 0, 0); // Felrood bij noodtoestand / hitte
  } 
  else if (vpdTooDry) {
    statusLed.setColor(255, 100, 0); // Oranje/Rood knipperend of vast bij uitdroging
    
    // Telegram waarschuwing bij uitdroging (max 1x per 10 minuten om spam te voorkomen)
    static unsigned long lastDryAlert = 0;
    if (millis() - lastDryAlert > 600000 && ENABLE_TELEGRAM && isConnected) {
      lastDryAlert = millis();
      sendTelegramAlert("🔥 *ALARM DE KLEINE KAS:* " + moldReasonText);
      logToSyslogAndSerial(F("[ALARM] Uitdrogingsalarm verzonden via Telegram."));
    }
  }
  else if (vpdTooWet || condensRisk || humHigh) {
    statusLed.setColor(255, 128, 0); // Oranje bij schimmel/klamrisico
  }
  else {
    statusLed.setColor(0, 0, 0); // Alles koel en rustig -> LED uit
  }

  // UI / Display status iconen
  if (isClimateCrash) vpdStatusText = " `(🚨 NOODSTOP)`";
  else if (vpdTooWet) vpdStatusText = " `(⚠️ Te klam)`";
  else if (vpdTooDry) vpdStatusText = " `(🔥 Te droog)`";
  else vpdStatusText = " `(✅ Optimaal)`";

  if (condensRisk) dpMarginStatusText = " `(⚠️ Risico)`";
  else dpMarginStatusText = " `(✅ Veilig)`";

  // Binnen & Buiten berekeningen...
  if (!isnan(indoorSmoothedTemp) && !isnan(indoorSmoothedHum)) {
    float correctedIndoorTemp = indoorSmoothedTemp + INDOOR_TEMP_OFFSET;
    float correctedIndoorHum = indoorSmoothedHum + INDOOR_HUM_OFFSET;
    indoorDewPoint = calcDewPoint(correctedIndoorTemp, correctedIndoorHum);
    indoorVpd = calcVPD(correctedIndoorTemp, correctedIndoorHum);
    indoorDpMargin = correctedIndoorTemp - indoorDewPoint;
  }

  if (!isnan(outdoorTemp) && !isnan(outdoorHumidity)) {
    outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
    outdoorVpd = calcVPD(outdoorTemp, outdoorHumidity);
    outdoorDpMargin = outdoorTemp - outdoorDewPoint; 
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
// =========================================================================
// 8. VENTILATIE- EN WONING-ADVIESLOGICA (Kas vs. Menselijke Actie)
// =========================================================================
void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
  if (isnan(inTemp) || isnan(inHum)) return;

  if (isKasSleeping) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "💤 De kleine kas is in slaapstand. Klimaatregeling gepauzeerd.";
    return;
  }
  
  if (millis() < 180000) {
    kasAdvice = KAS_OFF;
    kasAdviceReason = "Systeem is aan het opstarten...";
    return;
  }

  KasVentState rawNewAdvice = KAS_OFF;

  bool isTooWarm       = (inTemp > GREENHOUSE_MAX_TEMP);
  bool isKritischHeet    = (inTemp > 28.0); 
  bool isTooWet        = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
  bool isTooDry        = (kasVpd > VPD_MAX_OPTIMAL);
  bool condensRisk     = (kasDpMargin < DP_MARGIN_MIN);

  // Veilige waardes ophalen voor de woning
  float curIndoorHum  = !isnan(indoorSmoothedHum) ? indoorSmoothedHum : 50.0;

  // Variabelen voor Telegram advies aan jou
  String userActionAdvice = "";

  if (isClimateCrash || isKritischHeet) {
    rawNewAdvice = GREENHOUSE_VENTILATE;
    kasAdviceReason = "🚨 KRITIEK: De kleine kas is te heet (" + String(inTemp, 1) + "°C)! Maximale geforceerde ventilatie.";
    userActionAdvice = "🏠 *Actie woning:* Zet eventuele ramen/buitendeuren open om hitte af te voeren.";
  }
  else if (isTooDry) {
    // Kas is te droog (door bijv. kweeklampen). We willen vochtigere binnenlucht aantrekken.
    rawNewAdvice = GREENHOUSE_VENTILATE; // Externe fans aan om binnenlucht door te trekken
    kasAdviceReason = "🔥 Te droog (VPD " + String(kasVpd, 2) + " kPa). De kleine kas trekt vochtigere binnenlucht aan (LV " + String(curIndoorHum, 1) + "%).";
    
    // Controleer of de woning zelf inmiddels ook te droog raakt door de kas
    if (curIndoorHum < 45.0) {
      userActionAdvice = "⚠️ *Actie woning:* De woning raakt ook te droog doordat de kas vocht wegtrekt. Zet een raam/buitendeur in de woning op een kier om de woning te verversen.";
    } else {
      userActionAdvice = "🟢 *Actie woning:* Geen actie nodig. De woning levert voldoende buffer.";
    }
  }
  else if (isTooWet || condensRisk) {
    rawNewAdvice = GREENHOUSE_VENTILATE;
    kasAdviceReason = "⚠️ De kleine kas is te klam / condensrisico. Lucht wordt uitgewisseld met de woning.";
    userActionAdvice = "🟢 *Actie woning:* Geen actie nodig.";
  }
  else if (isTooWarm) {
    rawNewAdvice = GREENHOUSE_VENTILATE;
    kasAdviceReason = "🌡️ De kleine kas is te warm (" + String(inTemp, 1) + "°C). Geforceerde koeling gestart.";
    userActionAdvice = "🟢 *Actie woning:* Geen actie nodig.";
  }
  else {
    rawNewAdvice = KAS_OFF;
    kasAdviceReason = "✅ Klimaat in de kleine kas is in balans; kiemgroentes draaien stabiel.";
    userActionAdvice = "🟢 *Actie woning:* Geen actie nodig.";
  }

  kasAdvice = applyAntiHuntFilter(rawNewAdvice);

  // --- TELEGRAM MELDINGEN MET STILTE-TIMER ---
  static unsigned long lastAdviceAlertTime = 0;
  const unsigned long ADVICE_ALERT_COOLDOWN = 900000; // 15 minuten stilte

  String fullTelegramMessage = "🌿 *KAS KLIMAAT UPDATE*\n\n" + kasAdviceReason + "\n\n" + userActionAdvice;

  if (kasAdvice != previousKasAdvice) {
    previousKasAdvice = kasAdvice;
    lastAdviceAlertTime = millis();
    logToSyslogAndSerialPrintf("[ADVICE] Nieuw advies: %s", kasAdviceReason.c_str());
    sendTelegramAlert(fullTelegramMessage);
  } 
  else if (kasAdvice != KAS_OFF && (millis() - lastAdviceAlertTime > ADVICE_ALERT_COOLDOWN)) {
    lastAdviceAlertTime = millis();
    logToSyslogAndSerialPrintf("[ADVICE HERINNERING] Advies houdt aan: %s", kasAdviceReason.c_str());
    sendTelegramAlert(fullTelegramMessage);
  }
}
// // =========================================================================
// // 8. VENTILATIE- EN Woning-ADVIESLOGICA (Kas-ESP vs. Menselijke Actie)
// // =========================================================================
// void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
//   if (isnan(inTemp) || isnan(inHum)) return;

//   if (isKasSleeping) {
//     kasAdvice = KAS_OFF;
//     kasAdviceReason = "💤 Kas is in slaapstand. Klimaatregeling gepauzeerd.";
//     return;
//   }
  
//   if (millis() < 180000) {
//     kasAdvice = KAS_OFF;
//     kasAdviceReason = "Systeem is aan het opstarten...";
//     return;
//   }

//   KasVentState rawNewAdvice = KAS_OFF;

//   bool isTooWarm       = (inTemp > GREENHOUSE_MAX_TEMP);
//   bool isKritischHeet    = (inTemp > 28.0); 
//   bool isTooWet        = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
//   bool isTooDry        = (kasVpd > VPD_MAX_OPTIMAL);
//   bool condensRisk     = (kasDpMargin < DP_MARGIN_MIN);

//   // --- DRIE-PARTIJEN VERGELIJKING (Kas vs Binnen vs Buiten) ---
//   // Haal veilige/beschikbare waardes op
//   float curIndoorVpd  = !isnan(indoorVpd) ? indoorVpd : 1.0;
//   float curIndoorHum  = !isnan(indoorSmoothedHum) ? indoorSmoothedHum : 50.0;
  
//   float curOutdoorVpd = !isnan(outdoorVpd) ? outdoorVpd : 1.0;
//   float curOutdoorHum = !isnan(outdoorHumidity) ? outdoorHumidity : 60.0;

//   // Bepaal welke bron het meest geschikt is om naar de kas te trekken
//   bool outdoorIsWetterThanIndoor = (curOutdoorHum > curIndoorHum);
//   bool outdoorIsDrierThanIndoor  = (curOutdoorHum < curIndoorHum);

//   // Variabelen voor Telegram advies aan jou
//   String userActionAdvice = "";

//   if (isClimateCrash || isKritischHeet) {
//     rawNewAdvice = GREENHOUSE_VENTILATE;
//     kasAdviceReason = "🚨 KRITIEK: Kas uit de hand gelopen (" + String(inTemp, 1) + "°C)! Maximale geforceerde ventilatie.";
//     userActionAdvice = "🏠 *Actie woning:* Zet eventuele ramen/buitendeuren open om hitte af te voeren.";
//   }
//   else if (isTooDry) {
//     // Kas is te droog (door bijv. kweeklampen). We zoeken vochtigere lucht.
//     if (curIndoorHum >= curOutdoorHum) {
//       // Binnenlucht is vochtiger dan buiten -> ESP trekt binnenlucht aan via externe fans
//       rawNewAdvice = GREENHOUSE_VENTILATE; // Externe fans aan om de bron aan te trekken
//       kasAdviceReason = "🔥 Te droog (VPD " + String(kasVpd, 2) + " kPa). ESP trekt vochtigere binnenlucht aan (LV " + String(curIndoorHum, 1) + "%).";
      
//       // Controleer of de woning zelf inmiddels ook te droog raakt door de kas
//       if (curIndoorHum < 45.0) {
//         userActionAdvice = "⚠️ *Actie woning:* De woning raakt ook te droog doordat de kas vocht wegtrekt. Zet een raam/buitendeur op een kier om de woning te verversen.";
//       } else {
//         userActionAdvice = "🟢 *Actie woning:* Geen actie nodig. De woning levert voldoende buffer.";
//       }
//     } else {
//       // Buitenlucht is stiekem vochtiger dan binnen
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "🔥 Te droog (VPD " + String(kasVpd, 2) + " kPa). Buitenlucht is vochtiger; ESP ventileert met buitenlucht.";
//       userActionAdvice = "🟢 *Actie woning:* Geen actie nodig (ESP gebruikt buitenlucht).";
//     }
//   }
//   else if (isTooWet || condensRisk) {
//     // Kas is te klam. We zoeken drogere lucht.
//     if (curIndoorVpd > curOutdoorVpd && curIndoorHum < inHum) {
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "⚠️ Kas te klam / condensrisico. ESP ventileert naar de woning (drogere binnenlucht, VPD " + String(curIndoorVpd, 2) + ").";
      
//       if (curIndoorHum > 65.0) {
//         userActionAdvice = "🪟 *Actie woning:* De woning wordt te klam door de kaslucht! Zet de **buitendeur/raam van de woning** open om frisse buitenlucht binnen te laten.";
//       } else {
//         userActionAdvice = "🟢 *Actie woning:* Geen actie nodig.";
//       }
//     } else {
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "⚠️ Kas te klam / condensrisico. ESP ventileert direct naar buiten.";
//       userActionAdvice = "🟢 *Actie woning:* Geen actie nodig.";
//     }
//   }
//   else if (isTooWarm) {
//     rawNewAdvice = GREENHOUSE_VENTILATE;
//     kasAdviceReason = "🌡️ Kas te warm (" + String(inTemp, 1) + "°C). ESP start geforceerde koeling.";
//     userActionAdvice = "🟢 *Actie woning:* Geen actie nodig.";
//   }
//   else {
//     rawNewAdvice = KAS_OFF;
//     kasAdviceReason = "✅ Klimaat in de kas is in balans; kiemgroentes draaien stabiel.";
//     userActionAdvice = "🟢 *Actie woning:* Geen actie nodig.";
//   }

//   kasAdvice = applyAntiHuntFilter(rawNewAdvice);

//   // --- TELEGRAM MELDINGEN MET STILTE-TIMER ---
//   static unsigned long lastAdviceAlertTime = 0;
//   const unsigned long ADVICE_ALERT_COOLDOWN = 900000; // 15 minuten stilte

//   String fullTelegramMessage = "🌿 *KAS KLIMAAT UPDATE*\n\n" + kasAdviceReason + "\n\n" + userActionAdvice;

//   if (kasAdvice != previousKasAdvice) {
//     previousKasAdvice = kasAdvice;
//     lastAdviceAlertTime = millis();
//     logToSyslogAndSerialPrintf("[ADVICE] Nieuw advies: %s", kasAdviceReason.c_str());
//     sendTelegramAlert(fullTelegramMessage);
//   } 
//   else if (kasAdvice != KAS_OFF && (millis() - lastAdviceAlertTime > ADVICE_ALERT_COOLDOWN)) {
//     lastAdviceAlertTime = millis();
//     logToSyslogAndSerialPrintf("[ADVICE HERINNERING] Advies houdt aan: %s", kasAdviceReason.c_str());
//     sendTelegramAlert(fullTelegramMessage);
//   }
// }
// // =========================================================================
// // 8. VENTILATIE- EN VERWARMINGSLOGICA (Nu direct gericht op de Indoor-ruimte & Buitendeur)
// // =========================================================================
// void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
//   if (isnan(inTemp) || isnan(inHum)) return;

//   if (isKasSleeping) {
//     kasAdvice = KAS_OFF;
//     kasAdviceReason = "💤 Kas is in slaapstand. Klimaatregeling gepauzeerd.";
//     return;
//   }
  
//   if (millis() < 180000) {
//     kasAdvice = KAS_OFF;
//     kasAdviceReason = "Systeem is aan het opstarten...";
//     return;
//   }

//   KasVentState rawNewAdvice = KAS_OFF;

//   bool isTooWarm   = (inTemp > GREENHOUSE_MAX_TEMP);
//   bool isKritischHeet = (inTemp > 28.0); 
//   bool isTooWet    = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
//   bool isTooDry    = (kasVpd > VPD_MAX_OPTIMAL);
//   bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);

//   // Vergelijking: Is de woning beter dan de kas?
//   bool indoorIsFavorableForDryness = (!isnan(indoorSmoothedHum) && indoorSmoothedHum > (inHum + 2.0));
  
//   // Is de woning zelf ook slecht aan het worden door de kas-lucht? (Tijd om de gebruiker te vragen de buitendeur open te zetten)
//   bool indoorNeedsFreshAir = (!isnan(indoorSmoothedHum) && indoorSmoothedHum < 45.0);

//   if (isClimateCrash || isKritischHeet) {
//     rawNewAdvice = GREENHOUSE_VENTILATE; // Aangepast van KAS_OFF naar VENTILATE!
//     kasAdviceReason = "🚨 KRITIEK: Kas uit de hand gelopen (" + String(inTemp, 1) + "°C)! Maximale geforceerde ventilatie.";
//   }
//   else if (isTooDry) {
//     if (indoorIsFavorableForDryness) {
//       rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//       kasAdviceReason = "🔥 Te droog (VPD " + String(kasVpd, 2) + " kPa). ESP start ventilatie om vochtigere binnenlucht (LV " + String(indoorSmoothedHum, 1) + "%) door te trekken.";
      
//       if (indoorNeedsFreshAir) {
//         kasAdviceReason += " ⚠️ **Actie woning:** Binnenklimaat raakt ook uitgedroogd; zet de buitendeur van deze ruimte op een kier!";
//       }
//     } else {
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "🔥 Te droog (VPD " + String(kasVpd, 2) + " kPa). Binnenlucht biedt geen buffer; ESP start geforceerde luchtverversing.";
//     }
//   }
//   else if (isTooWet || condensRisk) {
//     rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//     kasAdviceReason = "⚠️️ Kas te klam / condensrisico. Interne circulatie gestart om microklimaat te breken.";
//   }
//   else if (isTooWarm) {
//     rawNewAdvice = GREENHOUSE_VENTILATE;
//     kasAdviceReason = "🌡️ Kas te warm (" + String(inTemp, 1) + "°C). ESP start geforceerde koeling.";
//   }
//   else {
//     rawNewAdvice = KAS_OFF;
//     kasAdviceReason = "✅ Klimaat in de kas is in balans; kiemgroentes draaien stabiel.";
//   }

//   kasAdvice = applyAntiHuntFilter(rawNewAdvice);

//   // Telegram meldingen sturen we pas als het advies verandert OF als het lang aanhoudt
//   static unsigned long lastAdviceAlertTime = 0;
//   const unsigned long ADVICE_ALERT_COOLDOWN = 900000; // 15 min stilte

//   if (kasAdvice != previousKasAdvice) {
//     previousKasAdvice = kasAdvice;
//     lastAdviceAlertTime = millis();
//     logToSyslogAndSerialPrintf("[ADVICE] Nieuw advies: %s", kasAdviceReason.c_str());
//     sendTelegramAlert("🌿 *KAS KLIMAAT UPDATE*\n\n" + kasAdviceReason);
//   }
//   else if (kasAdvice != KAS_OFF && (millis() - lastAdviceAlertTime > ADVICE_ALERT_COOLDOWN)) {
//     lastAdviceAlertTime = millis();
//     sendTelegramAlert("⏰ *KAS KLIMAAT HERINNERING*\n\n" + kasAdviceReason);
//   }
// }
// void updateVentilationAdvice(float inTemp, float inHum, float inDp, float outTemp, float outHum, float outDp) {
//   if (isnan(inTemp) || isnan(inHum)) return;

//   // 1. Check of de kas in slaapstand staat
//   if (isKasSleeping) {
//     kasAdvice = KAS_OFF;
//     kasAdviceReason = "💤 Kas is in slaapstand (isKasSleeping = true). Klimaatregeling is gepauzeerd.";
//     isHeatMatRecommended = false;
//     return;
//   }
  
//   if (millis() < 180000) {
//     kasAdvice = KAS_OFF;
//     kasAdviceReason = "Systeem is aan het opstarten...";
//     return;
//   }

//   // =========================================================================
//   // VPD TREND MONITORING
//   // =========================================================================
//   static float previousKasVpd = 0.0;
//   static unsigned long lastVpdCheckTime = 0;
//   unsigned long now = millis();
  
//   if (now - lastVpdCheckTime > 30000) {
//     float vpdDifference = kasVpd - previousKasVpd;
//     String trendStr = "STABIEL ➡️️";
//     if (vpdDifference > 0.01) trendStr = "STIJGT (wordt droger) 📈";
//     else if (vpdDifference < -0.01) trendStr = "DAALT (wordt vochtiger) 📉";
    
//     Serial.printf("[VPD MONITOR] Waarde: %.2f kPa | Trend: %s\n", kasVpd, trendStr.c_str());
//     previousKasVpd = kasVpd;
//     lastVpdCheckTime = now;
//   }

//   KasVentState rawNewAdvice = KAS_OFF;

//   // Grenzen & Situatie bepalen
//   bool isTooWarm   = (inTemp > GREENHOUSE_MAX_TEMP);
//   bool isKritischHeet = (inTemp > 28.0); 
//   bool isTooWet    = (kasVpd < VPD_MIN_OPTIMAL) || (inHum >= HUM_MOLD_THRESHOLD);
//   bool isTooDry    = (kasVpd > VPD_MAX_OPTIMAL);
//   bool condensRisk = (kasDpMargin < DP_MARGIN_MIN);

//   // --- SLIMME VERGELIJKING (De kern van jouw logica) ---
//   // Is de lucht binnenshuis gunstiger om uitdroging (te hoge VPD) te breken?
//   // (Bijv: indoor LV is hoger dan kas LV, of indoor VPD zit dichter bij het optimum dan kas VPD)
//   bool indoorIsFavorableForDryness = (!isnan(indoorSmoothedHum) && indoorSmoothedHum > (inHum + 2.0));

//   // --- HIERARCHISCHE REGELBESLISTHEID ---

//   if (isClimateCrash || isKritischHeet) {
//     rawNewAdvice = GREENHOUSE_VENTILATE;
//     kasAdviceReason = "🚨 KRITIEK: Kas te heet (" + String(inTemp, 1) + "°C)! Direct geforceerd koelen.";
//   }
//   // 1. TE DROOG (VPD te hoog): Heeft het zin om te ventileren?
//   else if (isTooDry) {
//     if (indoorIsFavorableForDryness) {
//       rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//       kasAdviceReason = "🔥 Te droog (VPD " + String(kasVpd, 2) + " kPa). Binnenlucht is gunstiger (LV " + String(indoorSmoothedHum, 1) + "%). 👉 **Actie:** ESP start ventilatie om vochtigere binnenlucht binnen te trekken!";
//     } else {
//       rawNewAdvice = GREENHOUSE_VENTILATE;
//       kasAdviceReason = "🔥 Te droog (VPD " + String(kasVpd, 2) + " kPa). Binnenlucht biedt geen verbetering, maar ESP start actieve luchtverversing om de hete lampen-lucht te breken.";
//     }
//   }
//   // 2. TE KLAM / SCHIMMEL- OF CONDENSRISICO
//   else if (isTooWet || condensRisk) {
//     rawNewAdvice = GREENHOUSE_CIRCULATE_INTERNAL;
//     kasAdviceReason = "⚠️ Kas te klam / condensrisico. Interne circulatie / ventilatie gestart om microklimaat te breken.";
//   }
//   // 3. TE WARM
//   else if (isTooWarm) {
//     rawNewAdvice = GREENHOUSE_VENTILATE;
//     kasAdviceReason = "🌡️ Kas te warm (" + String(inTemp, 1) + "°C). ESP start geforceerde ventilatie.";
//   }
//   // 4. BALANS
//   else {
//     rawNewAdvice = KAS_OFF;
//     kasAdviceReason = "✅ Klimaat in de kas is in balans; kiemgroentes draaien stabiel.";
//   }

//   kasAdvice = applyAntiHuntFilter(rawNewAdvice);

//   // --- TELEGRAM MELDINGEN MET STILTE-TIMER ---
//   static unsigned long lastAdviceAlertTime = 0;
//   const unsigned long ADVICE_ALERT_COOLDOWN = 900000; // 15 minuten stilte

//   if (kasAdvice != previousKasAdvice) {
//     previousKasAdvice = kasAdvice;
//     lastAdviceAlertTime = millis();
//     logToSyslogAndSerialPrintf("[ADVICE] Nieuw advies: %s", kasAdviceReason.c_str());
//     sendTelegramAlert("🌿 *KAS KLIMAAT UPDATE*\n\n" + kasAdviceReason);
//   } 
//   else if (kasAdvice != KAS_OFF && (millis() - lastAdviceAlertTime > ADVICE_ALERT_COOLDOWN)) {
//     lastAdviceAlertTime = millis();
//     logToSyslogAndSerialPrintf("[ADVICE HERINNERING] Advies houdt aan: %s", kasAdviceReason.c_str());
//     sendTelegramAlert("⏰ *KAS KLIMAAT HERINNERING*\n\n" + kasAdviceReason);
//   }
// }

// =========================================================================
// 8B. VERWARMINGSLOGICA (Warmtemat)
// =========================================================================
void updateHeatingAdvice(float inTemp, float outTemp) {
  if (isnan(inTemp) || isTestModeActive || isKasSleeping) return; 
  (void)outTemp;

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
// 9. HISTORIE & STATISTIEKEN
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
    }
  }
}

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

// =========================================================================
// 11. ERROR RATE CALCULATOR
// =========================================================================
float calculateErrorRate(unsigned long total, unsigned long failed) {
  if (total == 0) return 0.0;
  return ((float)failed / (float)total) * 100.0;
}
