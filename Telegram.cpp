/**
 * @file Telegram.cpp
 * @brief Volledige implementatie van Telegram bot interactie v3.4.0 met opgeschoonde fan-commando's, kalibratietoggle en klimaattests.
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
extern bool isClimateCrash;
extern bool isFakeEspOverheatActive;
extern void checkEspTemperatureSafety();
extern bool triggerManualShtHeater();
extern void setFansToMinTest();

// Vlag voor kalibratiemodus toggle
bool isCalibrationModeActive = false;
unsigned long calibrationStartTime = 0;
const unsigned long CALIBRATION_DURATION = 180000; // 3 minuten in milliseconden

// =========================================================================
// EASTER EGG: Unieke kiemgroenten-feitjes en humor
// =========================================================================
const char* microgreensEasterEggs[] = {
    "🌱 *Wist je dat:* Broccolikiemen tot wel 50 keer meer sulforafaan bevatten dan volwassen broccoliroosjes? Pure anti-oxidantenbom!",
    "🌿 *Kiem-wijsheid:* Zonnebloem kiemgroenten smaken heerlijk naar verse noten, maar vergeet niet de zaadhulpen tijdig weg te spoelen tegen schimmelvorming!",
    "💡 *Easter Egg:* Als je te hard naar je radijskiemen blaast, gaan ze niet sneller groeien, maar worden ze wel verlegen.",
    "🌱 *Feitje:* Erwtenkiemen (Pea Shoots) groeien zo snel dat je ze bijna kunt horen 'drinken' als je VPD-waarde exact op 0.70 kPa staat.",
    "🪴 *Pro-tip:* Te veel water = natte voeten = ellende. Houd die wortels vochtig, niet verzopen!",
    "🌿 *Weetje:* Luzerne (Alfalfa) wortelt zo diep in je mat dat ze bijna naar Australië proberen te boren.",
    "🌱 *Kiem-humor:* Wat is de favoriete muziek van een kiemgroente? Soul- en funk-muziek, vanwege al die wortel-grooves.",
    "🪴 *Feitje:* Radijskiemen hebben een pittige 'bite' omdat ze glucosinolaten aanmaken om insecten af te schrikken. Jouw smaakpapillen vinden dat gelukkig lekker.",
    "🌱 *Weetje:* Mosterdkiemen groeien zo snel dat ze binnen een week al klaar zijn om je boterham te veroveren.",
    "🌿 *Kiem-wijsheid:* Licht is pas nodig *nadat* je kiemen uit hun donkere kiemfase komen. Voordien willen ze alleen maar rust en duisternis.",
    "💡 *Easter Egg:* De ESP32-S3 in De kleine kas droomt 's nachts van elektrische schapen en perfecte VPD-curves.",
    "🌱 *Feitje:* Tuinkers was al populair bij de oude Romeinen. Die wisten blijkbaar al dat je geen hectare grond nodig hebt voor een vitamineshot.",
    "🪴 *Weetje:* Als je kiemgroenten te dicht op elkaar zaait, krijgen ze claustrofobie en vallen ze massaal om (damping-off). Geef ze ruimte!",
    "🌿 *Kiem-humor:* Waarom won de taugé de hardloopwedstrijd? Omdat hij in no-time ontkiemde!",
    "🌱 *Feitje:* Boekweitkiemen zien er futuristisch uit en smaken een beetje aards. Ideaal voor astronauten in een kleine kas.",
    "🪴 *Weetje:* Rode kool kiemen hebben prachtige paarse steeltjes. Ze zien er op je bord uit als kunstwerkjes.",
    "🌿 *Kiem-wijsheid:* Een stabiele temperatuur rond de 20 graden houdt je kiemen gelukkig en je energierekening laag.",
    "💡 *Easter Egg:* Dit systeem heeft meer liefde voor jouw kiemgroenten dan je eigen wekker voor jouw ochtendhumeur.",
    "🌱 *Feitje:* Kiemgroenten zijn eigenlijk 'baby-plantjes' op hun piekwaarde van vitamine-concentratie.",
    "🪴 *Slotfeit:* De beste kas is een automatische kas. Terwijl jij dit leest, regelt De kleine kas de rest wel weer!"
};

const int totalEasterEggs = sizeof(microgreensEasterEggs) / sizeof(microgreensEasterEggs[0]);

const char* getNextMicrogreenEasterEgg() {
    static int eggIndex = 0;
    const char* selectedEgg = microgreensEasterEggs[eggIndex];
    eggIndex = (eggIndex + 1) % totalEasterEggs; 
    return selectedEgg;
}

void sendTelegramAlert(String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!fanAlertsEnabled) {
    if (message.indexOf("ventilator") != -1 || message.indexOf("FAN") != -1 || message.indexOf("Fan") != -1) {
      return;
    }
  }

  yield();

  unsigned long timeSinceLastMsg = millis() - lastTelegramSentTime;
  if (timeSinceLastMsg < TELEGRAM_MIN_INTERVAL) {
    delay(50); 
  }

  if (bot.sendMessage(telid, message, "Markdown")) {
    lastTelegramSentTime = millis();
    digitalWrite(LED_BUILTIN, LOW);   
    delay(100);                        
    digitalWrite(LED_BUILTIN, HIGH);  
  }
}

// =========================================================================
// 📊 /st - STATUSRAPPORT
// =========================================================================
String buildStatusReport() {
  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
  int fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

  String modeStatus = "🟢 Normale Klimaatregeling";
  if (isKasSleeping) {
    modeStatus = "😴 Slaapstand Actief (Rust)";
  } else if (isCalibrationModeActive) {
    modeStatus = "⚖️ Kalibratiemodus Actief";
  } else if (!isSystemReady()) {
    modeStatus = "⏳ Systeem Kalibreert / Opstarten";
  }

  String kasActieLogica = kasAdviceReason;
  if (isHeatMatRecommended) kasActieLogica += "\nWarmtemat (Kas): AAN 🔥";
  else kasActieLogica += "\nWarmtemat (Kas): UIT 💤";

  String actieVereist = "🟢 Geen actie nodig. De kleine kas kan het helemaal zelf aan!";
  if (isKasSleeping) {
    actieVereist = "😴 Geen actie nodig, de kas is in slaapstand.";
  } else if (isCalibrationModeActive) {
    actieVereist = "⚖️ Kalibratie actief. Typ `/cal` om terug te keren naar automatische regeling.";
  } else if (!isSystemReady()) {
    actieVereist = "⏳ Even geduld: Systeem kalibreert nog.";
  } else if (moldRisk) {
    if (outdoorDewPoint >= indoorDewPoint) {
      actieVereist = "🔴 Schimmelrisico, MAAR buitenlucht is te klam (dauwpunt hoog). **Houd ramen dicht!**";
    } else {
      actieVereist = "🏠🪟 **Jouw actie vereist:** Zet het raam/binnendeur open om drogere bufferlucht naar de kas te leiden.";
    }
  } else if (homeVentActive) {
    actieVereist = "🏠🪟 **Jouw actie vereist:** Ventileer de woningruimte om drogere buitenlucht aan te voeren voor de buffer.";
  } else if (kasVpd > 1.0f) {
    actieVereist = "💡 *Toelichting:* De VPD is tijdelijk verhoogd door lampwarmte. Handmatige actie heeft geen zin; het microklimaat corrigeert zichzelf zodra de warmte daalt of lampen doven.";
  }

  String kasVpdStr    = (kasVpd <= -900.0) ? "N/A" : String(kasVpd, 2) + " kPa";
  String kasTempStr   = (displayedTemp <= -900.0) ? "N/A" : String(displayedTemp, 1) + "°C";
  String kasHumStr    = (kasSmoothedHum <= -900.0) ? "N/A" : String(kasSmoothedHum, 0) + "%";

  String indoorVpdStr  = (indoorVpd <= -900.0) ? "N/A" : String(indoorVpd, 2) + " kPa";
  String indoorTempStr = (indoorSmoothedTemp <= -900.0) ? "N/A" : String(indoorSmoothedTemp, 1) + "°C";
  String indoorHumStr  = (indoorHum <= -900.0) ? "N/A" : String(indoorHum, 0) + "%";

  String outdoorVpdStr = (outdoorVpd <= -900.0) ? "N/A" : String(outdoorVpd, 2) + " kPa";
  String outdoorTempStr = (outdoorTemp <= -900.0) ? "N/A" : String(outdoorTemp, 1) + "°C";
  String outdoorHumStr  = (outdoorHumidity <= 0.0 && outdoorTemp <= -900.0) ? "N/A" : String(outdoorHumidity, 0) + "%";

  static char reportBuf[1800];
  snprintf(reportBuf, sizeof(reportBuf),
    "📊 *Statusrapport 'De Kleine Kas'*\n\n"
    "⚙️ *Modus:* %s\n\n"
    "🌡️ *1. Huidige Waarden (VPD / Temp / LV):*\n"
    "• Kas:        `%s` | `%s` | `%s`\n"
    "• Woning: `%s` | `%s` | `%s`\n"
    "• Buiten:   `%s` | `%s` | `%s`\n\n"
    "⚙️ *3. Wat doet De kleine kas?*\n"
    "%s\n\n"
    "👤 *4. Wat moet JIJ doen (Woning-actuator)?*\n"
    "%s",
    modeStatus.c_str(),
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
         "• Status (/st) - Bekijk direct alle sensorwaarden\n"
         "• Minmax (/mm) - Min/max dagtemperaturen\n\n"
         
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
         "• Kalibratiemodus (/cal) - Fans off, handmatige SHT31 zelf kalibratie\n"
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
  else if (text == "pe" || text == "paasei") {
    String eggMessage = "🥚 *Geheime Kiem-Easter Egg*\n\n" + String(getNextMicrogreenEasterEgg());
    sendTelegramAlert(eggMessage);
  }
  else if (text == "advies" || text == "ad") {
    String kasHeader = "KLIMAAT OPTIMAAL";
    String kasKleur = "🟢";
    String kasDiagnose = kasAdviceReason;

    if (isKasSleeping) {
      kasHeader = "SLAAPSTAND";
      kasKleur = "⚪";
      kasDiagnose = "De kleine kas rust. Sensoren meten door, ventilatie is uitgeschakeld.";
    } else if (isCalibrationModeActive) {
      kasHeader = "KALIBRATIEMODUS";
      kasKleur = "⚖️";
      kasDiagnose = "Kalibratiemodus is actief. Alle ventilatoren staan stil.";
    } else if (!isSystemReady()) {
      kasHeader = "KALIBREREN";
      kasKleur = "🟡";
      kasDiagnose = "Systeem is aan het opstarten en de sensorfilters vullen zich.";
    } else if (isClimateCrash) {
      kasHeader = "NOODTOESTAND / HITTEPIEK";
      kasKleur = "🔴";
      kasDiagnose = "Kritieke temperatuur gedetecteerd! Maximale ventilatie actief.";
    } else if (moldRisk) {
      kasHeader = "SCHIMMELRISICO / TE KLAM";
      kasKleur = "🔴";
      kasDiagnose = moldReasonText;
    }

    String woningKleur = "🟢";
    String woningDiagnose = "Woningklimaat is geschikt als ventilatiebuffer.";
    String actieVoorJij = "Geen handmatige actie nodig, De kleine kas regelt het zelf.";

    if (isKasSleeping) {
      woningKleur = "⚪";
      woningDiagnose = "Slaapstand actief.";
      actieVoorJij = "Geen actie nodig, de kas slaapt.";
    } else if (isCalibrationModeActive) {
      woningKleur = "⚪";
      woningDiagnose = "Kalibratiemodus actief.";
      actieVoorJij = "Typ `/cal` om kalibratie uit te schakelen.";
    } else if (!isSystemReady()) {
      woningKleur = "🟡";
      woningDiagnose = "Systeem kalibreert nog.";
      actieVoorJij = "Even geduld: Systeem kalibreert nog.";
    } else if (homeVentActive) {
      woningKleur = "🟢";
      woningDiagnose = "Woningventilatie open aanbevolen (Buitenlucht kan ontvochtigen op basis van dauwpunt).";
      actieVoorJij = "Ventileer de woningruimte om drogere buitenlucht (lager dauwpunt) aan te voeren.";
    } else if (kasVpd > 1.0f) {
      actieVoorJij = "Geen actie zinvol; het microklimaat corrigeert zichzelf naarmate de warmte afneemt. Je kunt het met een gerust hart zo laten tot de lampen doven.";
    }

    String buitenKleur = "🟢";
    String buitenStatus = "Geschikt om in te zetten als ventilatielucht (op basis van fysieke vochtpotentie).";

    if (hasOutdoorAlert) {
      buitenKleur = "🟠"; 
      buitenStatus = "WEERALARM (" + weatherDesc + ")! \n  👉 *Waarom:* Buitenlucht is stormachtig/extreem; ramen dicht houden.";
    } else if (outdoorDewPoint >= indoorDewPoint && outdoorHumidity > 90.0) {
      buitenKleur = "🔴";
      buitenStatus = "Ongunstig (Hoge dauwpuntwaarde: " + String(outdoorDewPoint, 1) + "°C).\n  👉 *Waarom:* Buitenlucht toevoegen verhoogt het vochtgehalte in huis/kas.";
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
    adviesReport += "  _Diagnose:_ " + kasDiagnose + "\n";
    adviesReport += "• " + woningKleur + " *Woning:* " + woningDiagnose + "\n";
    adviesReport += "• " + buitenKleur + " *Buiten:* " + buitenStatus + "\n\n";
    adviesReport += "👤 *Wat moet JIJ doen?*\n";
    adviesReport += "• " + actieVoorJij + "\n\n";
    adviesReport += hwKleur + " *Hardware Status*\n";
    adviesReport += "• " + hwStatus;

    sendTelegramAlert(adviesReport);
  }
  else if (text == "minmax" || text == "mm") {
    String kasTempLowStr  = (kasTempLow <= -900.0 || kasTempLow >= 900.0)   ? "N/A" : String(kasTempLow, 1) + "°C";
    String kasTempHighStr = (kasTempHigh <= -900.0 || kasTempHigh >= 900.0) ? "N/A" : String(kasTempHigh, 1) + "°C";
    String kasHumLowStr   = (kasLowHum < 0.0 || kasLowHum > 100.0)         ? "N/A" : String(kasLowHum, 1) + "%";
    String kasHumHighStr  = (kasHighHum < 0.0 || kasHighHum > 100.0)       ? "N/A" : String(kasHighHum, 1) + "%";

    String indTempLowStr  = (indoorTempLow <= -900.0 || indoorTempLow >= 900.0)   ? "N/A" : String(indoorTempLow, 1) + "°C";
    String indTempHighStr = (indoorTempHigh <= -900.0 || indoorTempHigh >= 900.0) ? "N/A" : String(indoorTempHigh, 1) + "°C";
    String indHumLowStr   = (indoorLowHum < 0.0 || indoorLowHum > 100.0)         ? "N/A" : String(indoorLowHum, 1) + "%";
    String indHumHighStr  = (indoorHighHum < 0.0 || indoorHighHum > 100.0)       ? "N/A" : String(indoorHighHum, 1) + "%";

    String outTempLowStr  = (outdoorTempMin <= -900.0 || outdoorTempMin >= 900.0) ? "N/A" : String(outdoorTempMin, 1) + "°C";
    String outTempHighStr = (outdoorTempMax <= -900.0 || outdoorTempMax >= 900.0) ? "N/A" : String(outdoorTempMax, 1) + "°C";
    String outHumLowStr   = (outdoorLowHum < 0.0 || outdoorLowHum > 100.0)         ? "N/A" : String(outdoorLowHum, 1) + "%";
    String outHumHighStr  = (outdoorHighHum < 0.0 || outdoorHighHum > 100.0)       ? "N/A" : String(outdoorHighHum, 1) + "%";

    char mmBuffer[500];
    snprintf(mmBuffer, sizeof(mmBuffer),
      "📊 *Min / Max Extremen per Locatie*\n\n"
      "🪴 *Kas:*\n"
      "• Temp: `%s` | `%s`\n"
      "• LV:   `%s` | `%s`\n\n"
      "🏡 *Woning:*\n"
      "• Temp: `%s` | `%s`\n"
      "• LV:   `%s` | `%s`\n\n"
      "🌤️ *Buiten:*\n"
      "• Temp: `%s` | `%s`\n"
      "• LV:   `%s` | `%s`",
      kasTempLowStr.c_str(), kasTempHighStr.c_str(), kasHumLowStr.c_str(), kasHumHighStr.c_str(),
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
      currentTestDuration = 999999; // 999999 = unieke vlag voor oneindige sweep
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
      currentTestDuration = 86400000; // Oneindig vast tot herhaling commando
      
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
      sendTelegramAlert("⚖️ *Kalibratiemodus GEACTIVIONEERD (3 min)*\nFans staan stil voor offset-controle.\nSHT31 heater gestart.\nSchakelt over 3 minuten automatisch uit.");
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
    String indoorTempStr = (indoorSmoothedTemp <= -900.0) ? "N/A" : String(indoorSmoothedTemp, 1) + "°C";
    String indoorHumStr  = (indoorHum <= -900.0) ? "N/A" : String(indoorHum, 1) + "%";
    String pressureStr   = (currentPressure <= 0.0) ? "N/A" : String(currentPressure, 1) + " hPa";
    String luxStr        = (currentLuxValue < 0.0) ? "N/A" : String(currentLuxValue, 0) + " lx";
    String pirStatus     = (digitalRead(PIN_PIR) == HIGH) ? "Beweging 🚨" : "Rust 🟢";

    char hcBuf[1200];
    snprintf(hcBuf, sizeof(hcBuf),
      "🛠️ *Hardware & Sensor Check*\n\n"
      "⚙️ *Systeem:*\n"
      "• RAM: `%.1f KB` | Chip: `%.1f°C` 🧠\n\n"
      "🌿 *Kas:*\n"
      "• Temp: `%s` | LV: `%s` | Fout: `%.1f%%`\n\n"
      "🏡 *Woning:*\n"
      "• Temp: `%s` | LV: `%s`\n"
      "• Druk: `%s` | Fout: `%.1f%%`\n\n"
      "💡 *Overige Sensoren:*\n"
      "• Licht: `%s` (%s)\n"
      "• PIR: `%s`\n\n"
      "🌀 *Ventilatoren:*\n"
      "• Int:   `%d%%` (`%d RPM`)\n"
      "• Ext1: `%d%%` (`%d RPM`)\n"
      "• Ext2: `%d%%` (`%d RPM`)",
      heapKb, internalTemp,
      kasTempStr.c_str(), kasHumStr.c_str(), failKas,
      indoorTempStr.c_str(), indoorHumStr.c_str(), pressureStr.c_str(), failIndoor,
      luxStr.c_str(), lightMeterConnected ? "Online 🟢" : "Offline 🔴",
      pirStatus.c_str(),
      fanIntPct, fanIntRPM,
      fanExt1Pct, fanExt1RPM,
      fanExt2Pct, fanExt2RPM
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