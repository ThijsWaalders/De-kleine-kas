/**
 * @file Telegram.cpp
 * @brief Volledige implementatie van Telegram bot interactie v3.4.4 met persistent opgeslagen tray-modi (kiem, groei, warmtemat).
 */

#include "Telegram.h"
#include "Config.h"
#include "ClimateLogic.h"
#include "NetworkManager.h"
#include "EspSensors.h"
#include "MQTT.h"
#include "Logger.h"
#include <WiFi.h>

// Tellers voor nieuwe sensoren
extern unsigned long totalIndoorReads, failedIndoorReads;

// SHT31 sensor kalibratie
extern Preferences preferences;

// Externe variabelen en testfuncties uit andere modules
extern float espInternalTemp;
// Externe tray sensoren uit KasSensors
extern float traySensor1Temp;
extern float traySensor2Temp;
extern bool traySensor1Connected;
extern bool traySensor2Connected;
extern int tray1Mode; // 0 = Kiemfase, 1 = Groeifase, 2 = Warmtemat-modus
extern int tray2Mode;

extern bool isClimateCrash;
extern bool isFakeEspOverheatActive;
extern bool vpdRedZoneActive;

extern void checkEspTemperatureSafety();
extern bool triggerManualShtHeater();
extern void setFansToMinTest();

// Vlag voor kalibratiemodus toggle
bool isCalibrationModeActive = false;
unsigned long calibrationStartTime = 0;
const unsigned long CALIBRATION_DURATION = 180000; // 3 minuten in milliseconden


// Helper om het juiste status-icoon te bepalen op basis van de toestand
String getSystemStatusIcon() {
  if (isKasSleeping) return "⚪";                               // Rust / Slaap
  if (!isSystemReady() || isCalibrationModeActive) return "🟡"; // Opstarten of kalibreren
  if (isClimateCrash || moldRisk) return "🔴";                   // ACTIE VEREIST (Nood / Schimmel)
  
  // Kas heeft het zwaar (fans > 80% PWM of langdurige VPD overschrijding)
  if (fanExt1Speed > 200 || vpdRedZoneActive) return "🟠"; 
  
  // Kas stuurt actief bij en heeft het onder controle
  if (homeVentActive || kasVpd > VPD_MAX_OPTIMAL || kasVpd < VPD_MIN_OPTIMAL) return "🔵"; 

  return "🟢"; // Volledig in balans (Optimaal)
}

void sendTelegramAlert(String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  // Filter fan-alerts als ze zijn uitgeschakeld
  if (!fanAlertsEnabled) {
    if (message.indexOf("ventilator") != -1 || message.indexOf("FAN") != -1 || message.indexOf("Fan") != -1) {
      return;
    }
  }

  // 1. BEVEILIGING TEGEN TELEGRAM RATE-LIMIT: 
  // Als het vorige bericht te recent is, wachten we NIET met delay(), 
  // maar stellen we het verzenden heel even uit totdat het interval wel verstreken is.
  while (millis() - lastTelegramSentTime < TELEGRAM_MIN_INTERVAL) {
    yield(); // Geef de achtergrondtaken/WiFi de ruimte in plaats van de CPU stil te zetten
  }

  // 2. VERSTUREN BERICHT
  if (bot.sendMessage(telid, message, "Markdown")) {
    lastTelegramSentTime = millis();

    // 3. LED INDICATIE (Visuele feedback dat bericht verstuurd is)
    digitalWrite(LED_BUILTIN, LOW);   
    delay(15); // Korte, felle puls die wél goed zichtbaar is voor het oog
    digitalWrite(LED_BUILTIN, HIGH);  
  }
}
// void sendTelegramAlert(String message) {
//   if (WiFi.status() != WL_CONNECTED) return;

//   if (!fanAlertsEnabled) {
//     if (message.indexOf("ventilator") != -1 || message.indexOf("FAN") != -1 || message.indexOf("Fan") != -1) {
//       return;
//     }
//   }

//   yield();

//   unsigned long timeSinceLastMsg = millis() - lastTelegramSentTime;
//   if (timeSinceLastMsg < TELEGRAM_MIN_INTERVAL) {
//     delay(50); 
//   }

//   if (bot.sendMessage(telid, message, "Markdown")) {
//     lastTelegramSentTime = millis();
//     digitalWrite(LED_BUILTIN, LOW);   
//     delay(100);                        
//     digitalWrite(LED_BUILTIN, HIGH);  
//   }
// }

// Helper om traymodus tekst op te halen
String getTrayModeName(int mode) {
  if (mode == 0) return "Kiem 🫘";
  if (mode == 1) return "Groei 🌿";
  return "Warmtemat 🔥";
}

void checkAndSendHouseAdviceAlert() {
  static HouseVentState prevHouseAdvice = HOUSE_CLOSED;
  static unsigned long lastAdviceAlertTime = 0;

  // Alleen sturen bij een statuswijziging naar VENTILATE
  if (houseAdvice != prevHouseAdvice) {
    prevHouseAdvice = houseAdvice;
    if (houseAdvice == HOUSE_VENTILATE) {
      sendTelegramAlert("🏠 *Woning Ventilatie Advies:*\n" + houseAdviceReason);
      lastAdviceAlertTime = millis();
    } else {
      sendTelegramAlert("✅ *Woning Ventilatie Advies:* Woningklimaat is weer stabiel.");
    }
  }
  // Periodieke herinnering (elke 2 uur) zolang het advies actief blijft
  else if (houseAdvice == HOUSE_VENTILATE && (millis() - lastAdviceAlertTime >= 7200000UL)) {
    sendTelegramAlert("🏠 *Herinnering Woning Ventilatie:*\n" + houseAdviceReason);
    lastAdviceAlertTime = millis();
  }
}

// =========================================================================
// 📊 /st - STATUSRAPPORT
// =========================================================================
String buildStatusReport() {
  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
  int fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

  String statusIcon = getSystemStatusIcon();

  String modeStatus = "🟢 Normale Klimaatregeling";
  if (isKasSleeping) {
    modeStatus = "😴 Slaapstand";
  } else if (isCalibrationModeActive) {
    modeStatus = "⚖️ Kalibratiemodus";
  } else if (!isSystemReady()) {
    modeStatus = "⏳ Systeem kalibreert nog even..";
  }

  String kasActieLogica = kasAdviceReason;
  if (isHeatMatRecommended) kasActieLogica += "\nWarmtemat (Kas): AAN 🔥";
  else kasActieLogica += "\nWarmtemat (Kas): UIT 💤";

  // Bereken de dauwpuntmarge voor de veiligheidscheck
  float kasDpMargin = displayedTemp - kasDewPoint;

  String actieVereist = "🟢 Geen handmatige actie nodig. De kleine kas regelt het zelf.";
  if (isKasSleeping) {
    actieVereist = "😴 Ssst... de kleine kas slaapt.";
  } else if (isCalibrationModeActive) {
    actieVereist = "⚖️ Even wachten: Systeem kalibreert (fans staan stil).";
  } else if (!isSystemReady()) {
    actieVereist = "⏳ Even geduld: Systeem is nog aan het opstarten.";
  } else if (moldRisk || kasDpMargin < 2.0f) {
    if (outdoorDewPoint >= kasDewPoint) {
      actieVereist = "🛑 **DOE DEUR DICHT!** Er is schimmelrisico, MAAR de buitenlucht is te klam. Niet ventileren!";
    } else {
      actieVereist = "🪟 **DOE DEUR OPEN!** (dauwpuntmarge is te klein en de buitenlucht is droog genoeg).";
    }
  } else if (houseAdvice == HOUSE_VENTILATE) {
    actieVereist = "🏠 **VENTILEER WONING:** " + houseAdviceReason;
  } else if (homeVentActive) {
    actieVereist = "🟢 **Niets:** De kleine kas gebruikt automatisch de woningbuffer om bij te sturen.";
  } else if (indoorVpd < 0.80f) {
    actieVereist = "🟡 **Let op:** Woning-VPD is aan de klamme kant; buffer raakt vol.";
  } else {
    actieVereist = "🟢 **Niets:** Woningbuffer en kas-VPD zijn optimaal in balans.";
  }
  // // Bereken de dauwpuntmarge (Temperatuur - Dauwpunt) voor de veiligheidscheck
  // float kasDpMargin = displayedTemp - kasDewPoint;

  // String actieVereist = "🟢 Geen handmatige actie nodig. De kleine kas regelt het zelf.";
  // if (isKasSleeping) {
  //   actieVereist = "😴 Ssst... de kleine kas slaapt.";
  // } else if (isCalibrationModeActive) {
  //   actieVereist = "⚖️ Even wachten: Systeem kalibreert (fans staan stil).";
  // } else if (!isSystemReady()) {
  //   actieVereist = "⏳ Even geduld: Systeem is nog aan het opstarten.";
  // } else if (moldRisk || kasDpMargin < 2.0f) {
  //   if (outdoorDewPoint >= kasDewPoint) {
  //     actieVereist = "🛑 **DOE DEUR DICHT!** Er is schimmelrisico, MAAR de buitenlucht is te klam. Niet ventileren!";
  //   } else {
  //     actieVereist = "🪟 **DOE DEUR OPEN!** (dauwpuntmarge is te klein en de buitenlucht is droog genoeg).";
  //   }
  // } else if (homeVentActive) {
  //   actieVereist = "🟢 **Niets:** De kleine kas gebruikt automatisch de woningbuffer om bij te sturen.";
  // } else if (kasVpd > 1.0f) {
  //   actieVereist = "🟢 **Niets:** De kleine Kas is tikje droog, maar de dauwpuntmarge is veilig ruim. Stelt bij.";
  // }

  String kasVpdStr    = (kasVpd <= -900.0) ? "N/A" : String(kasVpd, 2) + " kPa";
  String kasTempStr   = (displayedTemp <= -900.0) ? "N/A" : String(displayedTemp, 1) + "°C";
  String kasHumStr    = (kasSmoothedHum <= -900.0) ? "N/A" : String(kasSmoothedHum, 0) + "%";
  
  String tray1Str     = (traySensor1Temp <= -900.0) ? "N/A" : String(traySensor1Temp, 1) + "°C";
  String tray1ModeStr = getTrayModeName(tray1Mode);
  
  String tray2Str     = (traySensor2Temp <= -900.0) ? "N/A" : String(traySensor2Temp, 1) + "°C";
  String tray2ModeStr = getTrayModeName(tray2Mode);

  String indoorVpdStr  = (indoorVpd <= -900.0) ? "N/A" : String(indoorVpd, 2) + " kPa";
  String indoorTempStr = (indoorSmoothedTemp <= -900.0) ? "N/A" : String(indoorSmoothedTemp, 1) + "°C";
  String indoorHumStr  = (indoorHum <= -900.0) ? "N/A" : String(indoorHum, 0) + "%";

  String outdoorVpdStr = (outdoorVpd <= -900.0) ? "N/A" : String(outdoorVpd, 2) + " kPa";
  String outdoorTempStr = (outdoorTemp <= -900.0) ? "N/A" : String(outdoorTemp, 1) + "°C";
  String outdoorHumStr  = (outdoorHumidity <= 0.0 && outdoorTemp <= -900.0) ? "N/A" : String(outdoorHumidity, 0) + "%";

  static char reportBuf[1900];
  snprintf(reportBuf, sizeof(reportBuf),
    "%s *Statusrapport 'De Kleine Kas'*\n\n"
    "⚙️ *Modus:* %s\n\n"
    "🌡️ *1. Huidige Waarden (VPD / Temp / LV):*\n"
    "• Tray 1:     `%s` [%s]\n"
    "• Tray 2:     `%s` [%s]\n"
    "• Kas:        `%s` | `%s` | `%s`\n"
    "• Woning: `%s` | `%s` | `%s`\n"
    "• Buiten:   `%s` | `%s` | `%s`\n\n"
    "⚙️ *3. Wat doet De kleine kas?*\n"
    "%s\n\n"
    "👤 *4. Wat moet jij doen (als woning-actuator)?*\n"
    "%s",
    statusIcon.c_str(),
    modeStatus.c_str(),
    tray1Str.c_str(), tray1ModeStr.c_str(),
    tray2Str.c_str(), tray2ModeStr.c_str(),
    kasVpdStr.c_str(), kasTempStr.c_str(), kasHumStr.c_str(),
    indoorVpdStr.c_str(), indoorTempStr.c_str(), indoorHumStr.c_str(),
    outdoorVpdStr.c_str(), outdoorTempStr.c_str(), outdoorHumStr.c_str(),
    kasActieLogica.c_str(),
    actieVereist.c_str()
  );

  return String(reportBuf);
}

// =========================================================================
// MENU
// =========================================================================
String buildHelpMenu() {
  return "🤖 *Beschikbare commando's:*\n\n"
         "• Advies (/ad) - Uitgebreid ventilatie & schimmeladvies\n"
         "• Kweekadvies (/ka) - Beste tijdstip met onderbouwing\n"
         "• Status (/st) - Bekijk direct alle sensorwaarden\n"
         "• Minmax (/mm) - Min/max dagtemperaturen\n\n"
         
         "• TS1 Kiemfase (/ts1k) - TS1 kiemmodus (17.5-23.5°C)\n"
         "• TS1 Groeifase (/ts1g) - TS1 groeimodus (15-25°C)\n"
         "• TS1 Warmtemat (/ts1wm) - TS1 warmtemat-modus (max 29°C)\n"
         "• TS1 Uit (/ts1off) - TS1 uitgeschakeld\n"
         "• TS2 Kiemfase (/ts2k) - TS2 kiemmodus (17.5-23.5°C)\n"
         "• TS2 Groeifase (/ts2g) - TS2 groeimodus (15-25°C)\n"
         "• TS2 Warmtemat (/ts2wm) - TS2 warmtemat-modus (max 29°C)\n"
         "• TS2 Uit (/ts2off) - TS2 uitgeschakeld\n\n"

         "• Test Sweep (/tfs) - Sweep alle fans (MIN ↔ MAX)\n"
         "• Test Low RPM (/tfl) - Alle 3 fans op MIN-PWM\n"
         "• Test High RPM (/tfh) - Alle 3 fans op MAX-PWM\n"
         "• Test Fan Vent (/tfv) - IntFan uit, extFans MAX-PWM\n\n"
         
         "• Test Uitdroging (/tu) - Simuleer uitdrogingsrisico\n"
         "• Test Klam (/tk) - Simuleer klamme kas / schimmelrisico\n"
         "• Test Woning (/tw) - Simuleer woning ventilatie buffer\n"
         "• Test Schimmel (/ts) - Schimmeltoggle (10 min)\n"
         "• Test Mat (/tm) - Test warmtemat (15 sec)\n"
         "• Test Chip Temp (/tc) - Test chip temp oververhitting\n"
         "• Kalibratiemodus (/cal) - Fans off, SHT31 kalibratie\n"
         "• Fan Alarm (/fa) - Fan alarmering aan/uit\n\n"
         
         "• Netwerkinfo (/ni) - Toon IP, RSSI en netwerkstatus\n"
         "• Hardwarecheck (/hc) - Meetwaardes hardware\n"
         "• Flush (/fl) - Wis Telegram wachtrij\n"
         "• Start AP/OTA (/ota) - Start Access Point voor OTA\n"
         "• Kas-slaap (/sl) - Zet kas op ruststand\n"
         "• Reboot (/rb) - Herstart het systeem\n"
         "• Help (/h) - Dit menu\n";
}

void sendHelpMenu() {
  sendTelegramAlert(buildHelpMenu());
}

// =========================================================================
// INKOMENDE BERICHTEN VERWERKEN
// =========================================================================
void handleTelegramIncoming() {
  if (millis() - lastBotCheckTime <= BOT_CHECK_INTERVAL) return;
  lastBotCheckTime = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  yield(); 
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1); 
  if (numNewMessages <= 0) return;
  yield(); 

  int lastIdx = numNewMessages - 1;
  bot.last_message_received = bot.messages[lastIdx].update_id;

  String senderChatId = String(bot.messages[lastIdx].chat_id);
  if (senderChatId != telid && telid != "") {
    logToSyslogAndSerial("[TELEGRAM] Onbekende afzender chat_id: " + senderChatId);
    return;
  }

  String text = bot.messages[lastIdx].text;
  if (text.startsWith("/")) {
    text.remove(0, 1);
  }
  text.trim();
  text.toLowerCase();

  logToSyslogAndSerial("[TELEGRAM] Commando ontvangen: " + text);

  if (text == "help" || text == "start" || text == "h") {
    sendHelpMenu();
  }
  else if (text == "status" || text == "st") {
    sendTelegramAlert(buildStatusReport());
  }
  else if (text == "ts1k") {
    tray1Mode = 0;
    sendMqttData();
    saveTrayModesToPreferences();
    sendTelegramAlert("🫘 *Tray 1 Modus:* Ingesteld op **Kiemfase** (17.5°C - 23.5°C) en opgeslagen in geheugen.");
  }
  else if (text == "ts1g") {
    tray1Mode = 1;
    sendMqttData();
    saveTrayModesToPreferences();
    sendTelegramAlert("🌿 *Tray 1 Modus:* Ingesteld op **Groeifase** (15.0°C - 25.0°C) en opgeslagen in geheugen.");
  }
  else if (text == "ts1wm") {
    tray1Mode = 2;
    sendMqttData();
    saveTrayModesToPreferences();
    sendTelegramAlert("🔥 *Tray 1 Modus:* Ingesteld op **Warmtemat-modus** (max 29.0°C) en opgeslagen in geheugen.");
  }
  else if (text == "ts2k") {
    tray2Mode = 0;
    sendMqttData();
    saveTrayModesToPreferences();
    sendTelegramAlert("🫘 *Tray 2 Modus:* Ingesteld op **Kiemfase** (17.5°C - 23.5°C) en opgeslagen in geheugen.");
  }
  else if (text == "ts2g") {
    tray2Mode = 1;
    sendMqttData();
    saveTrayModesToPreferences();
    sendTelegramAlert("🌿 *Tray 2 Modus:* Ingesteld op **Groeifase** (15.0°C - 25.0°C) en opgeslagen in geheugen.");
  }
  else if (text == "ts2wm") {
    tray2Mode = 2;
    sendMqttData();
    saveTrayModesToPreferences();
    sendTelegramAlert("🔥 *Tray 2 Modus:* Ingesteld op **Warmtemat-modus** (max 29.0°C) en opgeslagen in geheugen.");
  }
  else if (text == "ts1off") {
    tray1Mode = 3;
    sendMqttData();
    saveTrayModesToPreferences();
    sendTelegramAlert("🔌 *Tray 1 Modus:* Ingesteld op **Uit** (uitgeschakeld).");
  }
  else if (text == "ts2off") {
    tray2Mode = 3;
    sendMqttData();
    saveTrayModesToPreferences();
    sendTelegramAlert("🔌 *Tray 2 Modus:* Ingesteld op **Uit** (uitgeschakeld).");
  }
  else if (text == "kweekadvies" || text == "ka") {
    String ktReport = "🕒 *Advies Slimme Kweektijden*\n\n";
    
    // Check of er überhaupt verlichting nodig is
    bool needLighting = (tray1Mode != 0 || tray2Mode != 0);
    
    if (!needLighting) {
      ktReport += "🌱 *Status:* Alle trays staan in Kiemfase.\n";
      ktReport += "💡 *Advies:* Lampen blijven UIT. Kiemgroenten hebben nu alleen rust en duisternis nodig.";
    } else {
      ktReport += "⭐️ *Aanbevolen venster:* `20:00 - 12:00` (16u licht)\n\n";
      ktReport += "🔍 *Waarom dit venster?*\n";
      ktReport += "• *Nachtkoeling:* Woningbuffer is 's nachts koeler (" + String(indoorSmoothedTemp, 1) + "°C), waardoor lampwarmte de kleine kas perfect op temperatuur houdt.\n";
      ktReport += "• *Voorkom Hittepiek:* Overdag stijgt de kamertemperatuur; extra lampwarmte dwingt de fans tot maximaal zwoegen.\n";
      ktReport += "• *VPD Stabiliteit:* Minder schommelingen in de luchtvochtigheid tijdens de nachtelijke uren.";
    }

    sendTelegramAlert(ktReport);
  }
  else if (text == "advies" || text == "ad") {
    String kasHeader = "KLIMAAT OPTIMAAL";
    String kasKleur = getSystemStatusIcon();
    String kasDiagnose = kasAdviceReason;

    if (kasKleur == "🟢") {
      kasHeader = "KLIMAAT IS OPTIMAAL";
    } else if (kasKleur == "🔵") {
      kasHeader = "ONDER CONTROLE: KAS REGELT BIJ";
    } else if (kasKleur == "🟠") {
      kasHeader = "HOGE BELASTING: KAS ZWEET";
    } else if (kasKleur == "🔴") {
      kasHeader = "ACTIE VEREIST: NOOD/SCHIMMEL RISICO!";
    }

    String woningKleur = "🟢";
    String woningDiagnose = "Woningklimaat is geschikt als ventilatiebuffer (VPD: " + String(indoorVpd, 2) + " kPa).";
    String actieVoorJij = "Geen handmatige actie nodig, De kleine kas regelt het zelf.";

    if (isKasSleeping) {
      woningKleur = "⚪";
      woningDiagnose = "Slaapstand...";
      actieVoorJij = "Geen actie nodig, de kleine kas slaapt.";
    } else if (isCalibrationModeActive) {
      woningKleur = "⚪";
      woningDiagnose = "Kalibratiemodus";
      actieVoorJij = "Typ `/cal` om kalibratie uit te schakelen.";
    } else if (!isSystemReady()) {
      woningKleur = "🟡";
      woningDiagnose = "Systeem kalibreert nog.";
      actieVoorJij = "Even geduld: Systeem kalibreert nog.";
    } else if (houseAdvice == HOUSE_VENTILATE) {
      woningKleur = "🟠";
      woningDiagnose = "Woningbuffer moet gelucht worden: " + houseAdviceReason;
      actieVoorJij = "Zet woningventilatie/raam open (buitenlucht is fysiek droger).";
    } else if (indoorVpd < 0.80f) {
      woningKleur = "🟡";
      woningDiagnose = "Woning-VPD is laag (" + String(indoorVpd, 2) + " kPa); buffer dreigt te verzadigen.";
      actieVoorJij = "Bereid woningventilatie voor om de buffer te herstellen.";
    } else {
      actieVoorJij = "De kleine kas beheerst het klimaat via de woningbuffer.";
    }

    // String woningKleur = "🟢";
    // String woningDiagnose = "Woningklimaat is geschikt als ventilatiebuffer.";
    // String actieVoorJij = "Geen handmatige actie nodig, De kleine kas regelt het zelf.";

    // if (isKasSleeping) {
    //   woningKleur = "⚪";
    //   woningDiagnose = "Slaapstand...";
    //   actieVoorJij = "Geen actie nodig, de kleine kas slaapt.";
    // } else if (isCalibrationModeActive) {
    //   woningKleur = "⚪";
    //   woningDiagnose = "Kalibratiemodus";
    //   actieVoorJij = "Typ `/cal` om kalibratie uit te schakelen.";
    // } else if (!isSystemReady()) {
    //   woningKleur = "🟡";
    //   woningDiagnose = "Systeem kalibreert nog.";
    //   actieVoorJij = "Even geduld: Systeem kalibreert nog.";
    // } else if (homeVentActive) {
    //   woningKleur = "🟢";
    //   woningDiagnose = "Woningventilatie open aanbevolen (Buitenlucht kan ontvochtigen op basis van dauwpunt).";
    //   actieVoorJij = "Ventileer de woningruimte om drogere buitenlucht aan te voeren.";
    // } else if (kasVpd > 1.0f) {
    //   actieVoorJij = "De kleine kas corrigeert het klimaat mbt VPD naarmate de warmte afneemt.";
    // }

    String buitenKleur = "🟢";
    String buitenStatus = "Geschikt om in te zetten als ventilatielucht.";

    if (hasOutdoorAlert) {
      buitenKleur = "🟠"; 
      buitenStatus = "WEERALARM (" + weatherDesc + ")! \n  👉 *Waarom:* Buitenlucht is stormachtig/extreem; ramen dicht houden.";
    } else if (outdoorDewPoint >= indoorDewPoint && outdoorHumidity > 90.0) {
      buitenKleur = "🔴";
      buitenStatus = "Ongunstig (Hoge dauwpuntwaarde: " + String(outdoorDewPoint, 1) + "°C).";
    }

    float heapKb = ESP.getFreeHeap() / 1024.0F;
    float chipTemp = readEspInternalTemp();
    
    String hwKleur = "🟢";
    String hwStatus = "Systeem gezond en operationeel.";

    if (heapKb < 50.0F || chipTemp > 65.0F) {
      hwKleur = "🔴";
      hwStatus = "WAARSCHUWING:";
      if (heapKb < 50.0F) hwStatus += " RAM Laag (" + String(heapKb, 1) + " KB)!";
      if (chipTemp > 65.0F) hwStatus += " Chip Heet (" + String(chipTemp, 1) + "°C)!";
    }

    String adviesReport = "🌿 *Klimaat & Teeltadvies*\n\n";
    adviesReport += "🚦 *Status:*\n";
    adviesReport += "• " + kasKleur + " *Kas:* " + kasHeader + "\n";
    adviesReport += kasDiagnose + "\n";
    adviesReport += "• " + woningKleur + " *Woning:* " + woningDiagnose + "\n";
    adviesReport += "• " + buitenKleur + " *Buiten:* " + buitenStatus + "\n\n";
    adviesReport += hwKleur + " *Hardware Status*\n";
    adviesReport += "• " + hwStatus + "\n\n";
    adviesReport += "👤 *Wat moet jij doen?*\n";
    adviesReport += "• " + actieVoorJij + "";

    sendTelegramAlert(adviesReport);
  }
  else if (text == "minmax" || text == "mm") {
  String kasTempLowStr  = (kasTempLow <= -900.0 || kasTempHigh >= 900.0)   ? "N/A" : String(kasTempLow, 1) + "°C";
  String kasTempHighStr = (kasTempHigh <= -900.0 || kasTempHigh >= 900.0) ? "N/A" : String(kasTempHigh, 1) + "°C";
  String kasHumLowStr   = (kasLowHum < 0.0 || kasLowHum > 105.0)         ? "N/A" : String(kasLowHum, 1) + "%";
  String kasHumHighStr  = (kasHighHum < 0.0 || kasHighHum > 105.0)       ? "N/A" : String(kasHighHum, 1) + "%";

  // Tray min/max strings
  String tray1LowStr  = (traySensor1TempLow > 900.0)  ? "N/A" : String(traySensor1TempLow, 1) + "°C";
  String tray1HighStr = (traySensor1TempHigh < -900.0) ? "N/A" : String(traySensor1TempHigh, 1) + "°C";
  String tray2LowStr  = (traySensor2TempLow > 900.0)  ? "N/A" : String(traySensor2TempLow, 1) + "°C";
  String tray2HighStr = (traySensor2TempHigh < -900.0) ? "N/A" : String(traySensor2TempHigh, 1) + "°C";

  String indTempLowStr  = (indoorTempLow <= -900.0 || indoorTempHigh >= 900.0)   ? "N/A" : String(indoorTempLow, 1) + "°C";
  String indTempHighStr = (indoorTempHigh <= -900.0 || indoorTempHigh >= 900.0) ? "N/A" : String(indoorTempHigh, 1) + "°C";
  String indHumLowStr   = (indoorLowHum < 0.0 || indoorLowHum > 105.0)         ? "N/A" : String(indoorLowHum, 1) + "%";
  String indHumHighStr  = (indoorHighHum < 0.0 || indoorHighHum > 105.0)       ? "N/A" : String(indoorHighHum, 1) + "%";

  String outTempLowStr  = (outdoorTempMin <= -900.0 || outdoorTempMin >= 900.0) ? "N/A" : String(outdoorTempMin, 1) + "°C";
  String outTempHighStr = (outdoorTempMax <= -900.0 || outdoorTempMax >= 900.0) ? "N/A" : String(outdoorTempMax, 1) + "°C";
  String outHumLowStr   = (outdoorLowHum < 0.0 || outdoorLowHum > 105.0)         ? "N/A" : String(outdoorLowHum, 1) + "%";
  String outHumHighStr  = (outdoorHighHum < 0.0 || outdoorHighHum > 105.0)       ? "N/A" : String(outdoorHighHum, 1) + "%";

    char mmBuffer[600];
    snprintf(mmBuffer, sizeof(mmBuffer),
      "📊 *Min / Max Extremen per Locatie*\n\n"
      "🪴 *Kas & Trays:*\n"
      "• Kas Temp: `%s` | `%s`\n"
      "• Kas LV:   `%s` | `%s`\n"
      "• Tray 1:   `%s` | `%s`\n"
      "• Tray 2:   `%s` | `%s`\n\n"
      "🏡 *Woning:*\n"
      "• Temp: `%s` | `%s`\n"
      "• LV:   `%s` | `%s`\n\n"
      "🌤️ *Buiten:*\n"
      "• Temp: `%s` | `%s`\n"
      "• LV:   `%s` | `%s`",
      kasTempLowStr.c_str(), kasTempHighStr.c_str(), kasHumLowStr.c_str(), kasHumHighStr.c_str(),
      tray1LowStr.c_str(), tray1HighStr.c_str(),
      tray2LowStr.c_str(), tray2HighStr.c_str(),
      indTempLowStr.c_str(), indTempHighStr.c_str(), indHumLowStr.c_str(), indHumHighStr.c_str(),
      outTempLowStr.c_str(), outTempHighStr.c_str(), outHumLowStr.c_str(), outHumHighStr.c_str()
    );
    sendTelegramAlert(String(mmBuffer));
  }
  else if (text == "testmat" || text == "tm") {
    isTestModeActive = true;
    testModeStartTime = millis();
    currentTestDuration = 15000;
    isHeatMatRecommended = !isHeatMatRecommended;
    sendMqttData();
    sendTelegramAlert(isHeatMatRecommended ? "🧪 Warmtemat AAN (15s)" : "🧪 Warmtemat UIT (15s)");
  }
  else if (text == "testschimmel" || text == "ts") {
    isTestModeActive = true;
    testModeStartTime = millis();
    currentTestDuration = 600000;
    moldRisk = !moldRisk;
    sendMqttData();
    sendTelegramAlert(moldRisk ? "🧪 Schimmelrisico AAN (10m)" : "🧪 Schimmelrisico UIT (10m)");
  }
  else if (text == "testuitdroging" || text == "tu") {
    isTestModeActive = true;
    testModeStartTime = millis();
    currentTestDuration = 60000;
    kasVpd = 1.60;
    sendMqttData();
    sendTelegramAlert("🧪 *Test Uitdroging Actief (60s)*\nVPD geforceerd op 1.60 kPa (Uitdrogingsrisico).");
  }
  else if (text == "testklam" || text == "tk") {
    isTestModeActive = true;
    testModeStartTime = millis();
    currentTestDuration = 60000;
    kasVpd = 0.45;
    moldRisk = true;
    sendMqttData();
    sendTelegramAlert("🧪 *Test Klam / Schimmelrisico Actief (60s)*\nVPD geforceerd op 0.45 kPa + Schimmelrisico AAN.");
  }
  else if (text == "testwoning" || text == "tw") {
    isTestModeActive = true;
    testModeStartTime = millis();
    currentTestDuration = 60000;
    kasAdvice = GREENHOUSE_VENTILATE;
    kasAdviceReason = "🧪 Testmodus: Woninglucht inzetten als ventilatiebuffer.";
    sendMqttData();
    sendTelegramAlert("🧪 *Test Woning Ventilatie Actief (60s)*\nKas advies geforceerd op ventilatie via woningbuffer.");
  }
  else if (text == "testsweep" || text == "tfs") {
    if (isTestModeActive && currentTestDuration == 999999) {
      isTestModeActive = false;
      currentTestDuration = 0;
      sendTelegramAlert("🛑 *Fan Sweep Test handmatig gestopt.*\nSysteem draait weer automatisch op basis van sensoren.");
    } else {
      isTestModeActive = true;
      testModeStartTime = millis();
      currentTestDuration = 999999;
      sendTelegramAlert("🌀 *Fan Sweep Test gestart (Oneindig)*\nFans lopen traploos op en neer. _(Stuur nogmaals `/tfs` om te stoppen)_");
    }
  }
  else if (text == "testfanlow" || text == "tfl") {
    if (isTestModeActive && currentTestDuration == 86400000 && fanExt1Speed == EXT_FAN_MIN_PWM) {
      isTestModeActive = false;
      currentTestDuration = 0;
      sendTelegramAlert("🛑 *Low RPM Test handmatig gestopt.*\nSysteem draait weer automatisch op basis van sensoren.");
    } else {
      isTestModeActive = true;
      testModeStartTime = millis();
      currentTestDuration = 86400000;
      
      fanIntSpeed  = INT_FAN_MIN_PWM;
      fanExt1Speed = EXT_FAN_MIN_PWM; 
      fanExt2Speed = EXT_FAN_MIN_PWM; 
      fanIntPct    = map(fanIntSpeed,  0, 255, 0, 100);
      fanExt1Pct   = map(fanExt1Speed, 0, 255, 0, 100);
      fanExt2Pct   = map(fanExt2Speed, 0, 255, 0, 100);

      ledcWrite(PIN_FAN_INT_PWM,  fanIntSpeed);
      ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
      ledcWrite(PIN_FAN_EXT2_PWM, fanExt2Speed);

      sendMqttData();
      sendTelegramAlert("🌀 *Low RPM Test gestart*\nAlle fans op minimale PWM. _(Stuur nogmaals `/tfl` om te stoppen)_");
    }
  }
  else if (text == "testfanhigh" || text == "tfh") {
    if (isTestModeActive && currentTestDuration == 86400000 && fanExt1Speed == 255) {
      isTestModeActive = false;
      currentTestDuration = 0;
      sendTelegramAlert("🛑 *High RPM Test handmatig gestopt.*\nSysteem draait weer automatisch op basis van sensoren.");
    } else {
      isTestModeActive = true;
      testModeStartTime = millis();
      currentTestDuration = 86400000;
      
      fanIntSpeed  = 255; 
      fanExt1Speed = 255; 
      fanExt2Speed = 255;
      fanIntPct    = 100;
      fanExt1Pct   = 100;
      fanExt2Pct   = 100;

      ledcWrite(PIN_FAN_INT_PWM, 255);
      ledcWrite(PIN_FAN_EXT1_PWM, 255);
      ledcWrite(PIN_FAN_EXT2_PWM, 255);

      sendMqttData();
      sendTelegramAlert("🌀 *High RPM Test gestart*\nAlle fans op 100%. _(Stuur nogmaals `/tfh` om te stoppen)_");
    }
  }
  else if (text == "testfanvent" || text == "tfv") {
    if (isTestModeActive && currentTestDuration == 86400000 && fanIntSpeed == 0 && fanExt1Speed == 255) {
      isTestModeActive = false;
      currentTestDuration = 0;
      sendTelegramAlert("🛑 *Test Fan Vent handmatig gestopt.*\nSysteem draait weer automatisch op basis van sensoren.");
    } else {
      isTestModeActive = true;
      testModeStartTime = millis();
      currentTestDuration = 86400000;
      
      fanIntSpeed  = 0;              
      fanExt1Speed = 255;           
      fanExt2Speed = 255;
      fanIntPct    = 0;
      fanExt1Pct   = 100;
      fanExt2Pct   = 100;

      ledcWrite(PIN_FAN_INT_PWM, 0);              
      ledcWrite(PIN_FAN_EXT1_PWM, 255);           
      ledcWrite(PIN_FAN_EXT2_PWM, 255);

      sendMqttData();
      sendTelegramAlert("🌀 *Test Fan Vent (`tfv`) actief:* Interne fan UIT, Externe fans op maximaal. _(Stuur nogmaals `/tfv` om te stoppen)_");
    }
  }
  else if (text == "testchip" || text == "tc") {
    isFakeEspOverheatActive = !isFakeEspOverheatActive;
    checkEspTemperatureSafety(); 
    sendTelegramAlert(isFakeEspOverheatActive ? "🧪 *Test Chiptemperatuur Alarm AAN (72°C)*" : "✅ *Test Chiptemperatuur Alarm UIT*");
  }
  else if (text == "slaapstand" || text == "sl") {
    isKasSleeping = !isKasSleeping;
    preferences.begin("kas_config", false);
    preferences.putBool("sleeping", isKasSleeping);
    preferences.end();
    sendTelegramAlert(isKasSleeping ? "😴 *De kleine kas is in slaapstand gezet.*" : "🌅 *De kleine kas is ontwaakt!*");
  }
  else if (text == "kalibratie" || text == "kal" || text == "cal") {
    if (isCalibrationModeActive) {
      isCalibrationModeActive = false;
      sendTelegramAlert("✅ *Kalibratiemodus HANDMATIG UITGESCHAKELD*\nAutomatische klimaatregeling hervat.");
    } else {
      isCalibrationModeActive = true;
      calibrationStartTime = millis();

      fanIntSpeed = 0;
      fanExt1Speed = 0;
      fanExt2Speed = 0;
      isHeatMatRecommended = false;
      ledcWrite(PIN_FAN_INT_PWM, 0);
      ledcWrite(PIN_FAN_EXT1_PWM, 0);
      ledcWrite(PIN_FAN_EXT2_PWM, 0);
      
      if (!lampsAreOn) {
        triggerManualShtHeater(); 
      }
      sendTelegramAlert("⚖️ *Kalibratiemodus GEACTIVIONEERD (3 min)*\nFans staan stil voor offset-controle.\nSHT31 heater gestart.");
    }
  }
  else if (text == "hwcheck" || text == "hc") {
    float heapKb = ESP.getFreeHeap() / 1024.0F;
    float failKas = (totalKasReads > 0) ? ((float)failedKasReads / totalKasReads) * 100.0 : 0.0;
    float failIndoor = (totalIndoorReads > 0) ? ((float)failedIndoorReads / totalIndoorReads) * 100.0 : 0.0;

    int fanIntPct  = map(fanIntSpeed, 0, 255, 0, 100);
    int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
    int fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);
    float internalTemp = readEspInternalTemp();

    String kasTempStr    = (displayedTemp <= -900.0) ? "N/A" : String(displayedTemp, 1) + "°C";
    String kasHumStr     = (kasSmoothedHum <= -900.0) ? "N/A" : String(kasSmoothedHum, 1) + "%";
    String tray1Str      = (traySensor1Temp <= -900.0) ? "N/A" : String(traySensor1Temp, 1) + "°C";
    String tray2Str      = (traySensor2Temp <= -900.0) ? "N/A" : String(traySensor2Temp, 1) + "°C";
    
    String tray1ModeStr  = getTrayModeName(tray1Mode);
    String tray2ModeStr  = getTrayModeName(tray2Mode);
    
    String indoorTempStr = (indoorSmoothedTemp <= -900.0) ? "N/A" : String(indoorSmoothedTemp, 1) + "°C";
    String indoorHumStr  = (indoorHum <= -900.0) ? "N/A" : String(indoorHum, 1) + "%";
    String pressureStr   = (currentPressure <= 0.0) ? "N/A" : String(currentPressure, 1) + " hPa";
    String luxStr        = (currentLuxValue < 0.0) ? "N/A" : String(currentLuxValue, 0) + " lx";
    String pirStatus     = (digitalRead(PIN_PIR) == HIGH) ? "Beweging 🚨" : "Rust 🟢";

    String statusIcon = getSystemStatusIcon();

    char hcBuf[1300];
    snprintf(hcBuf, sizeof(hcBuf),
      "%s *Hardware & Sensor Check*\n\n"
      "🌿 *Kas & Trays:*\n"
      "• Temp: `%s` | LV: `%s` | Fout: `%.1f%%`\n"
      "• Tray 1: `%s` (%s) [%s]\n"
      "• Tray 2: `%s` (%s) [%s]\n\n"
      "🏡 *Woning:*\n"
      "• Temp: `%s` | LV: `%s`\n"
      "• Druk: `%s` | Fout: `%.1f%%`\n\n"
      "💡 *Overige Sensoren:*\n"
      "• Licht: `%s` (%s)\n"
      "• PIR: `%s`\n\n"
      "🌀 *Ventilatoren:*\n"
      "• Int:   `%d%%` (`%d RPM`)\n"
      "• Ext1: `%d%%` (`%d RPM`)\n"
      "• Ext2: `%d%%` (`%d RPM`)\n\n"
      "⚙️ *Systeem:*\n"
      "• RAM: `%.1f KB` | Chip: `%.1f°C` 🧠\n\n",
      statusIcon.c_str(),
      kasTempStr.c_str(), kasHumStr.c_str(), failKas,
      tray1Str.c_str(), traySensor1Connected ? "Online 🟢" : "Offline / Vocht 🔴", tray1ModeStr.c_str(),
      tray2Str.c_str(), traySensor2Connected ? "Online 🟢" : "Offline / Vocht 🔴", tray2ModeStr.c_str(),
      indoorTempStr.c_str(), indoorHumStr.c_str(), pressureStr.c_str(), failIndoor,
      luxStr.c_str(), lightMeterConnected ? "Online 🟢" : "Offline 🔴",
      pirStatus.c_str(),
      fanIntPct, fanIntRPM,
      fanExt1Pct, fanExt1RPM,
      fanExt2Pct, fanExt2RPM,
      heapKb, internalTemp
    );
    sendTelegramAlert(String(hcBuf));
  }
  else if (text == "fanalarm" || text == "fa") {
    fanAlertsEnabled = !fanAlertsEnabled;
    sendTelegramAlert(fanAlertsEnabled ? "🔔 *Fan-alarm ingeschakeld.*" : "🔕 *Fan-alarm uitgeschakeld (gedempt).*");
  }
  else if (text == "netwerkinfo" || text == "ni") {
    long rssi = WiFi.RSSI();
    String signalQuality = (rssi > -70) ? "Uitstekend 🟢" : ((rssi > -85) ? "Matig 🟡" : "Slecht 🔴");

    char niBuf[500];
    snprintf(niBuf, sizeof(niBuf),
      "🌐 *Netwerk- & Systeeminfo*\n\n"
      "• *Firmware:* `%s`\n"
      "• *Gebouwd op:* `%s`\n"
      "• *WiFi:* Verbonden ✅\n"
      "• *SSID:* `%s`\n"
      "• *IP-adres:* `%s`\n"
      "• *Signaal:* `%ld dBm` (%s)\n"
      "• *Gateway:* `%s`",
      FIRMWARE_VERSION, BUILD_TIMESTAMP,
      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
      rssi, signalQuality.c_str(), WiFi.gatewayIP().toString().c_str()
    );
    sendTelegramAlert(String(niBuf));
  }
  else if (text == "flush" || text == "reflush" || text == "fl") {
    bot.getUpdates(-1);
    sendTelegramAlert("🧹 *Wachtrij opgeschoond!*");
  }
  else if (text == "ota") {
    String apMsg = "🚀 *Access Point wordt gestart...*\n\n";
    apMsg += "• *SSID:* `" + String(AP_SSID) + "`\n";
    apMsg += "• *Wachtwoord:* `" + String(AP_PASS) + "`\n\n";
    apMsg += "• Verbind je PC hiermee en stuur de OTA update.";

    bot.last_message_received = bot.messages[lastIdx].update_id;
    bot.getUpdates(bot.last_message_received + 1);
    
    bot.sendMessage(senderChatId, apMsg, "Markdown");
    triggerOtaApMode = true; 
  }
  else if (text == "reboot" || text == "rb") {
    bot.sendMessage(telid, "⚠ Systeem wordt herstart...", "");
    bot.last_message_received = bot.messages[lastIdx].update_id;
    bot.getUpdates(bot.last_message_received + 1);
    delay(1000); 
    ESP.restart();
  }
  else {
    sendTelegramAlert("❌ Onbekend commando. Typ /h of /help");
  }
}

void updateCalibrationMode() {
  if (isCalibrationModeActive) {
    if (millis() - calibrationStartTime >= CALIBRATION_DURATION) {
      isCalibrationModeActive = false;
      sendTelegramAlert("⚖️ *Kalibratiemodus automatisch beëindigd.*\nAutomatische klimaatregeling is hervat.");
    }
  }
}