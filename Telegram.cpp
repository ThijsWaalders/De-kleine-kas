/**
 * @file Telegram.cpp
 * @brief Geoptimaliseerde implementatie van Telegram bot interactie met veilige rate-limiting en behoud van alle commando's voor ESP32-S3.
 */

#include "Telegram.h"
#include "Config.h"
#include "ClimateLogic.h"
#include "NetworkManager.h"
#include "MQTT.h"
#include "Logger.h"
#include <WiFi.h>
#include <esp_random.h>

// Tellers voor nieuwe sensoren
extern unsigned long totalIndoorReads, failedIndoorReads;

// Laat Telegram.cpp weten dat deze variabele ergens anders bestaat
extern float espInternalTemp;

// BUFFER VOOR TELEGRAM RAPPORTEN
char advBuffer[1600];

// =========================================================================
// EASTER EGG: Unieke kiemgroenten-feitjes en humor (Vergrendeld voor /pe)
// =========================================================================
const char* microgreensEasterEggs[] = {
    "🌱 *Wist je dat:* Broccolikiemen tot wel 50 keer meer sulforafaan bevatten dan volwassen broccoliroosjes? Pure anti-oxidantenbom!",
    "🌿 *Kiem-wijsheid:* Zonnebloemkiemgroenten smaken heerlijk naar verse noten, maar vergeet niet de zaadhulpen tijdig weg te spoelen tegen schimmelvorming!",
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
    "🪴 *Weetje:* Rode kool kiemen hebben prachtige paarse steeltjes. Ze zien er op je bord uit alsof ze uit een kunstgalerie komen.",
    "🌿 *Kiem-wijsheid:* Een stabiele temperatuur rond de 20 graden houdt je kiemen gelukkig en je energierekening laag.",
    "💡 *Easter Egg:* Dit systeem heeft meer liefde voor jouw kiemgroenten dan je eigen wekker voor jouw ochtendhumeur.",
    "🌱 *Feitje:* Kiemgroenten zijn eigenlijk 'baby-plantjes' op hun piekwaarde van vitamine-concentratie. Je eet ze op hun sportiefste leeftijd!",
    "🪴 *Slotfeit:* De beste kas is een automatische kas. Terwijl jij dit leest, regelt De kleine kas de rest wel weer!"
};

const int totalEasterEggs = sizeof(microgreensEasterEggs) / sizeof(microgreensEasterEggs[0]);

const char* getNextMicrogreenEasterEgg() {
    static int eggIndex = -1;
    if (eggIndex == -1) {
      // Gebruik Hardware TRNG (True Random Number Generator) van ESP32 bij de eerste keer
      eggIndex = esp_random() % totalEasterEggs;
    } else {
      eggIndex = (eggIndex + 1) % totalEasterEggs;
    }
    return microgreensEasterEggs[eggIndex];
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

String formatVal(float val, int decimals, const char* unit) {
  if (val <= -900.0 || (strcmp(unit, "hPa") == 0 && val <= 0.0)) return "N/A";
  char buf[32];
  if (decimals == 0) snprintf(buf, sizeof(buf), "%.0f %s", val, unit);
  else if (decimals == 2) snprintf(buf, sizeof(buf), "%.2f %s", val, unit);
  else snprintf(buf, sizeof(buf), "%.1f %s", val, unit);
  return String(buf);
}

// =========================================================================
// 📊 /st - STATUSRAPPORT
// =========================================================================
String buildStatusReport() {
  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
  int fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

  bool anyFanActive = (fanExt1Pct > 0 || fanExt2Pct > 0 || fanIntPct > 0);

  String kasActieLogica = "🟢 Kas-klimaat is optimaal en stabiel.";
  if (!isSystemReady()) {
    kasActieLogica = "⏳ Systeem is aan het opstarten / kalibreren.";
  } else if (kasVpd < VPD_MIN_OPTIMAL) {
    kasActieLogica = moldRisk ? "🌧️ Schimmelrisico in kas (De kleine kas stuurt bij)." : "💧 Kas aan de klamme kant (De kleine kas regelt actief).";
  } else if (kasVpd > VPD_MAX_OPTIMAL) {
    if (kasAdvice == GREENHOUSE_VENTILATE) {
      kasActieLogica = "🔥 Kas te droog; De kleine kas benut drogere woninglucht om te ventileren.";
    } else {
      kasActieLogica = "🔥 Uitdrogingsrisico in kas (De kleine kas houdt de kas gesloten om vocht vast te houden).";
    }
  } else if (anyFanActive || isHeatMatRecommended) {
    kasActieLogica = "🔵 De kleine kas voert actieve klimaatregulering uit.";
  }

  // Actuator details toevoegen
  kasActieLogica += isHeatMatRecommended ? "\nWarmtemat (Kas): AAN 🔥" : "\nWarmtemat (Kas): UIT 💤";

  if (kasAdvice == GREENHOUSE_VENTILATE) {
    kasActieLogica += "\nKas-ventilatie: Actief via Woninglucht 💨";
  } else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
    kasActieLogica += "\nKas-circulatie: Interne circulatie (breekt microklimaat) 🌀";
  } else {
    kasActieLogica += "\nKas-actuators: In rust / gesloten 🔒";
  }

  if (fanExt1Pct > 0 || fanExt2Pct > 0) {
    kasActieLogica += "\nExterne fans: Actief (" + String(fanExt1Pct) + "% / " + String(fanExt2Pct) + "%).";
  } else {
    kasActieLogica += "\nExterne fans: In rust.";
  }

  if (kasAdviceReason.length() > 0) {
    kasActieLogica += "\n*Toelichting:* " + kasAdviceReason;
  }

  String actieVereist = "🟢 Geen actie nodig. De kleine kas regelt alles volledig zelf.";
  if (!isSystemReady()) {
    actieVereist = "⏳ Even geduld: Systeem kalibreert nog.";
  } else if (moldRisk) {
    if (indoorHum > 85.0) {
      actieVereist = "🔴 Schimmelrisico, MAAR woninglucht is te klam (>85% LV). **Houd woning droog!**";
    } else {
      actieVereist = "🏠🪟 **Jouw actie vereist:** Ventileer de woning zodat er schone bufferlucht voor de kas is.";
    }
  }

  String currentIp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "Niet verbonden";

  static char reportBuf[1600];
  snprintf(reportBuf, sizeof(reportBuf),
    "📊 *Statusrapport 'De Kleine Kas'*\n\n"
    "🌐 *Netwerk:* IP: `%s`\n\n"
    "🌡️ *1. Huidige Waarden (VPD / Temp / LV):*\n"
    "• Kas: `%.2f kPa` | `%.1f°C` | `%.0f%%`\n"
    "• Woning: `%.2f kPa` | `%.1f°C` | `%.0f%%`\n"
    "• Buiten: `%.2f kPa` | `%.1f°C` | `%.0f%%`\n\n"
    "💧 *2. Dauwpunt (Kas):*\n"
    "• Dauwpunt: `%.1f°C` (Marge: `%.1f°C`)%s\n\n"
    "⚙️ *3. Wat doet De kleine kas?*\n"
    "%s\n\n"
    "👤 *4. Wat moet JIJ doen (Woning-actuator)?*\n"
    "%s",
    currentIp.c_str(),
    kasVpd, displayedTemp, kasSmoothedHum,
    indoorVpd, indoorSmoothedTemp, indoorHum,
    outdoorVpd, outdoorTemp, outdoorHumidity,
    kasDewPoint, (displayedTemp - kasDewPoint), dpMarginStatusText.c_str(),
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
         "• *Status* (`/st`) - Bekijk direct alle sensorwaarden\n"
         "• *Advies* (`/ad`) - Uitgebreid ventilatie & schimmeladvies\n"
         "• *Minmax* (`/mm`) - Hoogste/laagste dagtemperatuur\n"
         "• *Paasei* (`/pe`) - Random kiemgroenten-feitje of humor\n"
         "• *Testmat* (`/tm`) - Test warmtemat (15 sec)\n"
         "• *Testschimmel* (`/ts`) - Test schimmeltoggle (10 min)\n"
         "• *Testfans* (`/tf`) - Test alle fans op 100% (30 sec)\n"
         "• *Testfans 2* (`/tf2`) - Interne fan uit, ext. fans 1 & 2 naar max (30 sec)\n"
         "• *Manual Ext Fans 100%* (`/mf`) - Interne fan uit, ext. fans 1 & 2 naar max (ON/OFF)\n"
         "• *Kas-slaap* (`/sl`) - Zet kas op ruststand / gesloten\n"
         "• *Hardwarecheck* (`/hc`) - Meetwaardes hardware\n"
         "• *Fanalarm* (`/fa`) - Fan alarmering aan/uit\n"
         "• *Start AP/OTA* (`/ota`) - Start Access Point voor OTA\n"
         "• *Flush* (`/fl`) - Wis Telegram wachtrij\n"
         "• *Reboot* (`/rb`) - Herstart het systeem\n"
         "• *Help* (`/h`) - Dit menu\n";
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
  if (senderChatId != telid) return;

  String text = bot.messages[lastIdx].text;
  if (text.startsWith("/")) text.remove(0, 1);
  text.trim();
  text.toLowerCase();

  logToSyslogAndSerial("[TELEGRAM] Commando ontvangen: " + text);

  if (text == "help" || text == "start" || text == "h") {
    sendHelpMenu();
  }
  else if (text == "status" || text == "st") {
    sendTelegramAlert(buildStatusReport());
  }
  else if (text == "pe" || text == "paasei" || text == "ea") {
    String eggMessage = "🥚 *Geheime Kiem-Easter Egg*\n\n" + String(getNextMicrogreenEasterEgg());
    sendTelegramAlert(eggMessage);
  }
  else if (text == "advies" || text == "ad") {
    String statusIcon = "🟢 VEILIG KLIMAAT";
    String diagnoseWaarom = "Geen actie vereist. Klimaat is stabiel binnen de VPD-marges.";
    
    if (isKasSleeping) {
      statusIcon = "💤 SLAAPSTAND";
      diagnoseWaarom = "De kleine kas rust. Sensoren meten door, ventilatie uitgeschakeld.";
    } else if (!isSystemReady()) {
      statusIcon = "⏳ KALIBREREN";
      diagnoseWaarom = "Systeem is aan het opstarten en de sensorfilters vullen zich.";
    } else if (kasAdvice == GREENHOUSE_CRASH_TEMP) {
      statusIcon = "🔴 ZINLOOS / NOODKLIMATEN";
      diagnoseWaarom = "Extreme temperatuur- of vochtigheidspiek gedetecteerd in de kas!";
    } else if (moldRisk) {
      statusIcon = "🌧️ SCHIMMELRISICO / TE KLAM";
      diagnoseWaarom = moldReasonText;
    } else if (kasVpd < VPD_MIN_OPTIMAL) {
      statusIcon = "💧 KLAM / TE VOCHTIG";
      diagnoseWaarom = "Kasvochtigheid is aan de hoge kant, De kleine kas stuurt bij via Woninglucht.";
    } else if (kasAdvice == GREENHOUSE_VENTILATE) {
      statusIcon = "🟡 De kas stelt bij";
      diagnoseWaarom = kasAdviceReason;
    }

    String actieVoorJij = "Geen handmatige actie nodig, de kleine kas kan het nu zelf weer aan.";
    if (isKasSleeping) {
      actieVoorJij = "Wees stil en pak ook je rust, de kleine kas slaapt...";
    } else if (!isSystemReady()) {
      actieVoorJij = "Even geduld: Systeem kalibreert nog.";
    } else if (moldRisk || kasVpd < VPD_MIN_OPTIMAL) {
      if (indoorHum > 85.0) {
        actieVoorJij = "🔴 **Houd woningdroog!** Schimmelrisico ***EN*** Woning-LV is >85%.";
      } else {
        actieVoorJij = "🏠🪟 **Ventileer de woning** om drogere bufferlucht aan te voeren.";
      }
    }

    float heapKb = ESP.getFreeHeap() / 1024.0F;
    float chipTemp = readEspInternalTemp();

    static char advBufferLocal[1600];
    snprintf(advBufferLocal, sizeof(advBufferLocal),
      "🌿 *Klimaat & Teeltadvies*\n\n"
      "🚦 *Status:* %s\n"
      "• *Waarom:* %s\n\n"
      "👤 *Wat moet JIJ doen?*\n"
      "• %s\n\n"
      "🌍 *Buitenweer (API)*\n"
      "• Temp: `%.1f°C` | LV: `%.1f%%`\n"
      "• *Buitenlucht:* %s\n\n"
      "📈 *24-Uurs Extremen (Kas)*\n"
      "• Temp Min: `%.1f°C` | Max: `%.1f°C`\n"
      "• LV Min: `%.0f%%` | Max: `%.0f%%`\n\n"
      "⚙️ *Hardware & Gezondheid*\n"
      "• Vrij RAM: `%.1f KB` | Chip-Temp: `%.1f°C`\n"
      "• Systeem: %s",
      statusIcon.c_str(),
      diagnoseWaarom.c_str(),
      actieVoorJij.c_str(),
      outdoorTemp, outdoorHumidity,
      (outdoorHumidity < 85.0 ? "Geschikt om te ventileren ✅" : "Te klam/vochtig (>85%%), niet inzetten ❌"),
      kasTempLow, kasTempHigh,
      kasLowHum, kasHighHum,
      heapKb, chipTemp,
      (!isSystemReady() ? "⏳ Kalibreren" : "🟢 Operationeel")
    );
    
    sendTelegramAlert(String(advBufferLocal));
  }
  else if (text == "minmax" || text == "mm") {
    char mmBuffer[400];
    snprintf(mmBuffer, sizeof(mmBuffer),
      "📊 *Min / Max Extremen per Locatie*\n\n"
      "🪴 *Kas:*\n"
      "• Temp: L `%.1f°C` | H `%.1f°C`\n"
      "• LV: L `%.0f%%` | H `%.0f%%`\n\n"
      "🏡 *Woning:*\n"
      "• Temp: L `%.1f°C` | H `%.1f°C`\n"
      "• LV: L `%.0f%%` | H `%.0f%%`\n\n"
      "🌤️ *Buiten:*\n"
      "• Temp: L `%.1f°C` | H `%.1f°C`\n"
      "• LV: L `%.0f%%` | H `%.0f%%`",
      kasTempLow, kasTempHigh, kasLowHum, kasHighHum,
      indoorTempLow, indoorTempHigh, indoorLowHum, indoorHighHum,
      outdoorTempMin, outdoorTempMax, outdoorLowHum, outdoorHighHum
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
  else if (text == "testfans" || text == "tf") {
    isTestModeActive = true;
    testModeStartTime = millis();
    currentTestDuration = 30000;
    sendTelegramAlert("🌀 *Alle fans (2 extern + intern) in testmodus (30s op 100%)!*");
  }
  else if (text == "testfans2" || text == "tf2") {
    isTestModeActive = true;
    testModeStartTime = millis();
    currentTestDuration = 30000;
    
    fanIntSpeed = 0;              
    fanExt1Speed = 255;           
    fanExt2Speed = 255;           
    sendMqttData();
    
    sendTelegramAlert("🌀 *Testmodus `tf2` actief (30s):* Interne fan UIT, Externe fans 1 & 2 op maximaal vermogen!");
  }
  else if (text == "mf") {
    mfOverrideActive = !mfOverrideActive;
    
    if (mfOverrideActive) {
      fanIntSpeed = 0;
      fanExt1Speed = 255;
      fanExt2Speed = 255;
      
      ledcWrite(PIN_FAN_INT_PWM,  0);
      ledcWrite(PIN_FAN_EXT1_PWM, 255);
      ledcWrite(PIN_FAN_EXT2_PWM, 255);
      
      sendMqttData();      
      logToSyslogAndSerial("[TFM] Handmatige TFM override INGESCHAKELD.");
      sendTelegramAlert("🚨 *TFM OVERRIDE AAN*\nInterne fan is UITgeschakeld. Externe fans draaien **VOL GAS**!");
    } else {
      mfOverrideActive = false;
      sendMqttData();
      logToSyslogAndSerial("[TFM] Handmatige TFM override UITGESCHAKELD.");
      sendTelegramAlert("✅ *TFM OVERRIDE UIT*\nSysteem is teruggekeerd naar de automatische klimaatregeling.");
    }
  }
  else if (text == "sl") {
    isKasSleeping = !isKasSleeping;
    
    if (isKasSleeping) {
      fanIntSpeed = 0;
      fanExt1Speed = 0;
      fanExt2Speed = 0;
      isHeatMatRecommended = false;
      sendMqttData();
      sendTelegramAlert("💤 *Kas in Slaapstand (Kweek UIT)*\n\nKlimaatregeling is **uitgeschakeld**. Sensoren en weerdata meten gewoon door.");
    } else {
      sendTelegramAlert("🌱 *Kas Actief (Kweek AAN)*\n\nKlimaatregeling en automatische sturing zijn weer **ingeschakeld**!");
    }
  }
  else if (text == "reboot" || text == "rb") {
    bot.sendMessage(telid, "⚠ Systeem wordt herstart...", "");
    logToSyslogAndSerial(F("[TELEGRAM] Handmatige reboot aangevraagd. Wachtrij opschonen..."));

    bot.last_message_received = bot.messages[lastIdx].update_id;
    bot.getUpdates(bot.last_message_received + 1);

    delay(1500); 
    ESP.restart();
  }
  else if (text == "flush" || text == "reflush" || text == "fl") {
    int updates = bot.getUpdates(-1); 
    if (updates > 0) {
      bot.last_message_received = bot.messages[lastIdx].update_id;
      bot.getUpdates(bot.last_message_received + 1);
    }
    sendTelegramAlert("🧹 *Wachtrij opgeschoond!*");
  }
  else if (text == "hwcheck" || text == "hc") {
    float heapKb = ESP.getFreeHeap() / 1024.0F;
    
    float failKas = (totalKasReads > 0) ? ((float)failedKasReads / totalKasReads) * 100.0 : 0.0;
    float failIndoor = (totalIndoorReads > 0) ? ((float)failedIndoorReads / totalIndoorReads) * 100.0 : 0.0;

    int fanIntPct  = map(fanIntSpeed, 0, 255, 0, 100);
    int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
    int fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);
    
    float espInternalTemp = readEspInternalTemp();

    char hcBuf[1024];
    snprintf(hcBuf, sizeof(hcBuf),
      "🛠️ *Hardware & Sensor Check*\n\n"
      "⚙️ *Systeem:*\n"
      "• RAM: `%.1f KB` vrije ruimte\n"
      "• Chip-Temp: `%.1f°C` 🧠\n\n"
      "📊 *Sensoren:*\n"
      "• Lichtsterkte: `%.0f lx`\n"
      "• Kas (SHT31): Temp `%s` | LV `%s` (Fouten: `%.1f%%`)\n"
      "• Woning (BME280): Temp `%s` | `%.2f hPa`\n"
      "• Woning (BME LV): `%s` (Fouten: `%.1f%%`)\n\n"
      "🌀 *Fans (2 Extern + Intern):*\n"
      "• Intern (Circulatie): `%d%%` (`%d RPM`)\n"
      "• Extern 1 (Hoofd): `%d%%` (`%d RPM`)\n"
      "• Extern 2 (Boost): `%d%%` (`%d RPM`)\n",
      heapKb,
      espInternalTemp,
      currentLuxValue,
      formatVal(displayedTemp, 1, "°C").c_str(), formatVal(kasSmoothedHum, 1, "%").c_str(), failKas,
      formatVal(indoorSmoothedTemp, 1, "°C").c_str(), currentPressure,
      formatVal(indoorHum, 1, "%").c_str(), failIndoor,
      fanIntPct, fanIntRPM,
      fanExt1Pct, fanExt1RPM,
      fanExt2Pct, fanExt2RPM
    );
    sendTelegramAlert(String(hcBuf));
  }
  else if (text == "fanalarm" || text == "fa") {
    fanAlertsEnabled = !fanAlertsEnabled;

    String statusMsg = fanAlertsEnabled 
      ? "🔔 *Fan-alarm is weer INGESCHAKELD via Telegram.*" 
      : "🔕 *Fan-alarm is UITGESCHAKELD (gedempt) via Telegram.*";
      
    sendTelegramAlert(statusMsg);
  }
  else if (text == "ota") {
    String apMsg = "🚀 *Access Point wordt gestart...*\n\n";
    apMsg += "• *SSID:* `" + String(AP_SSID) + "`\n";
    apMsg += "• *Wachtwoord:* `" + String(AP_PASS) + "`\n\n";
    apMsg += "Verbind je PC hiermee en stuur de OTA update.";

    bot.last_message_received = bot.messages[lastIdx].update_id;
    bot.getUpdates(bot.last_message_received + 1);
    bot.sendMessage(senderChatId, apMsg, "Markdown");

    startAPMode();
  }
  else {
    sendTelegramAlert("❌ Onbekend commando. Typ /help");
  }
}

String getResetReasonString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "Power On";
    case ESP_RST_EXT:       return "External Pin";
    case ESP_RST_SW:        return "Software Reset";
    case ESP_RST_PANIC:     return "Exception / Panic";
    case ESP_RST_INT_WDT:   return "Internal WDT";
    case ESP_RST_TASK_WDT:  return "Task WDT";
    case ESP_RST_DEEPSLEEP: return "Deep Sleep Wake";
    case ESP_RST_BROWNOUT:  return "Brownout Reset";
    default:                return "Onbekend";
  }
}

void sendBootNotification() {
  if (WiFi.status() != WL_CONNECTED) return;

  String bootReason = getResetReasonString(esp_reset_reason());
  String currentSSID = WiFi.SSID();
  String currentIp = WiFi.localIP().toString();
  
  String telegramMsg;
  telegramMsg.reserve(130);
  telegramMsg = "🚀 *De kleine kas is opgestart*\n";
  telegramMsg += "⚙️ *Reden:* " + bootReason + "\n";
  telegramMsg += "📶 *SSID:* " + currentSSID + "\n";
  telegramMsg += "🌐 *IP:* `" + currentIp + "`";

  if (bot.sendMessage(telid, telegramMsg, "Markdown")) {
    logToSyslogAndSerial(F("[TELEGRAM] Opstartmelding succesvol verzonden."));
  }
}