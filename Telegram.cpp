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

// Externe tellers voor de 2e DHT (Woning)
extern unsigned long totalIndoorDhtReads;
extern unsigned long failedIndoorDhtReads;

// Laat Telegram.cpp weten dat deze variabele ergens anders bestaat
extern float espInternalTemp;

// BUFFER VOOR TELEGRAM RAPPORTEN
char advBuffer[1600];

/**
 * @brief Stuurt een bericht via de Telegram bot met ingebouwde veiligheid en fan-alarm filter.
 */
void sendTelegramAlert(String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  // --- FILTER VOOR FAN ALARMEN ---
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

/**
 * @brief Hulpfunctie om ontbrekende sensoren netjes weer te geven als N/A.
 */
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

  String regelingStatus = "🟢 Kas-klimaat is optimaal en stabiel.";
  if (millis() < SYSTEM_STARTUP_DELAY) {
    regelingStatus = "⏳ Systeem is aan het opstarten / kalibreren.";
  } else if (kasVpd < VPD_MIN_OPTIMAL) {
    regelingStatus = moldRisk ? "⚠️ Schimmelrisico in kas (ESP stuurt bij)." : "⚠️ Kas aan de klamme kant (ESP regelt).";
  } else if (kasVpd > VPD_MAX_OPTIMAL) {
    if (kasAdvice == GREENHOUSE_VENTILATE) {
      regelingStatus = "🔥 Kas te droog, maar ESP benut betere woninglucht om te corrigeren.";
    } else {
      regelingStatus = "🔥 Uitdrogingsrisico in kas (ESP houdt kas gesloten).";
    }
  } else if (anyFanActive || isHeatMatRecommended) {
    regelingStatus = "🔵 ESP voert actieve kas-regulering uit.";
  }

  String actuatorStatus = "";
  if (isHeatMatRecommended) actuatorStatus += "• Warmtemat (Kas): AAN 🔥\n";
  else actuatorStatus += "• Warmtemat (Kas): UIT 💤\n";

  if (kasAdvice == GREENHOUSE_VENTILATE) {
    actuatorStatus += "• Kas-ventilatie: ESP benut buiten-/woninglucht 💨\n";
  } else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
    actuatorStatus += "• Kas-circulatie: ESP breekt microklimaat 🌀\n";
  } else {
    actuatorStatus += "• Kas-actuators: In rust / gesloten 🔒\n";
  }

  if (fanExt1Pct > 0 || fanExt2Pct > 0) {
    actuatorStatus += "• Externe fans (ESP): Actief (" + String(fanExt1Pct) + "% / " + String(fanExt2Pct) + "%).";
  } else {
    actuatorStatus += "• Externe fans (ESP): In rust.";
  }

  String toelichtingTekst = kasAdviceReason;
  if (toelichtingTekst.length() == 0) {
    toelichtingTekst = "Geen actie vereist; de kas draait volledig zelfstandig.";
  }

  String actieVereist = "🟢 Geen actie nodig. De ESP regelt de kas volledig zelf.";
  if (millis() < SYSTEM_STARTUP_DELAY) {
    actieVereist = "⏳ Even geduld: Systeem kalibreert nog.";
  } else if (moldRisk) {
    if (outdoorHumidity > 85.0) {
      actieVereist = "🔴 Schimmelrisico, MAAR buiten/woning is te klam (>85% LV). **Houd ramen dicht!**";
    } else {
      actieVereist = "🏠🪟 **Jouw actie vereist:** Zet het raam/binnendeur open om drogere bufferlucht naar de kas te leiden.";
    }
  }

  String currentIp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "Niet verbonden";

  static char reportBuf[1600];
  snprintf(reportBuf, sizeof(reportBuf),
    "📊 *Statusrapport 'De Kleine Kas'*\n\n"
    "🌐 *Netwerk:* IP: `%s`\n\n"
    "🌡️ *1. Huidige Waarden:*\n"
    "• Kas: `%.1f°C` | LV: `%.0f%%`\n"
    "• Binnen: `%.1f°C` | LV: `%.0f%%`\n"
    "• Buiten: `%.1f°C` | LV: `%.0f%%`\n\n"
    "💧 *2. VPD & Dauwpunt (Kas):*\n"
    "• VPD: `%.2f kPa`%s\n"
    "• Dauwpunt: `%.1f°C` (Marge: `%.1f°C`)%s\n\n"
    "⚙️ *3. Wat doet de ESP (Kas-automatisering)?*\n"
    "%s\n"
    "%s\n\n"
    "💡 *4. Toelichting (Systeemlogica):*\n"
    "• %s\n\n"
    "👤 *5. Wat moet JIJ doen (Woning-actuator)?*\n"
    "%s",
    currentIp.c_str(),
    displayedTemp, kasSmoothedHum,
    indoorSmoothedTemp, indoorHum,
    outdoorTemp, outdoorHumidity,
    kasVpd, vpdStatusText.c_str(),
    kasDewPoint, (displayedTemp - kasDewPoint), dpMarginStatusText.c_str(),
    regelingStatus.c_str(),
    actuatorStatus.c_str(),
    toelichtingTekst.c_str(),
    actieVereist.c_str()
  );

  return String(reportBuf);
}

// =========================================================================
// MENU
// =========================================================================
String buildHelpMenu() {
  String helpMsg = "🤖 *Beschikbare commando's:*\n\n"
                   "• *Status* (`/st`) - Bekijk direct alle sensorwaarden\n"
                   "• *Advies* (`/ad`) - Uitgebreid ventilatie & schimmeladvies\n"
                   "• *Minmax* (`/mm`) - Hoogste/laagste dagtemperatuur\n"
                   "• *Testmat* (`/tm`) - Test warmtemat (15 sec)\n"
                   "• *Testschimmel* (`/ts`) - Test schimmeltoggle (10 min)\n"
                   "• *Testfans* (`/tf`) - Test alle fans op 100% (30 sec)\n"
                   "• *Testfans 2* (`/tf2`) - Interne fan uit, ext. fans 1 & 2 naar max (30 sec)\n"
                   "• *Manual Ext Fans 100%* (`/mf`) - Interne fan uit, ext. fans 1 & 2 naar max (ON/OFF)\n"
                   "• *Kas-slaap* (`/sleep`) - Zet kas op ruststand / gesloten\n"
                   "• *Hardwarecheck* (`/hc`) - Meetwaardes hardware\n"
                   "• *Fanalarm* (`/fa`) - Fan alarmering aan/uit\n"
                   "• *Start AP/OTA* (`/ota`) - Start Access Point voor OTA\n"
                   "• *Flush* (`/fl`) - Wis Telegram wachtrij\n"
                   "• *Reboot* (`/rb`) - Herstart het systeem\n"
                   "• *Help* (`/h`) - Dit menu\n";
  return helpMsg;
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
  else if (text == "advies" || text == "ad") {
    int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
    int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
    int fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

    bool anyFanActive = (fanExt1Pct > 0 || fanExt2Pct > 0 || fanIntPct > 0);

    // 1. Status, Kleur & Waarom
    String statusIcon = "🟢 VEILIG KLIMAAT";
    String diagnoseWaarom = "Klimaat is stabiel binnen de VPD-marges. Geen actie vereist.";
    
    if (isKasSleeping) {
      statusIcon = "💤 SLAAPSTAND";
      diagnoseWaarom = "De kweekruimte is in rust. Sensoren meten door, ventilatie en kweekactie zijn uitgeschakeld.";
    } else if (millis() < SYSTEM_STARTUP_DELAY) {
      statusIcon = "⏳ OPSTARTEN / KALIBREREN";
      diagnoseWaarom = "Systeem is aan het opstarten en de sensorfilters vullen zich.";
    } else if (kasAdvice == GREENHOUSE_CRASH_TEMP) {
      statusIcon = "🔴 KRITIEK / CLIMATE CRASH";
      diagnoseWaarom = "Extreme temperatuur- of vochtigheidspiek gedetecteerd in de kas!";
    } else if (moldRisk) {
      statusIcon = "🔴 SCHIMMELRISICO";
      diagnoseWaarom = moldReasonText;
    } else if (kasAdvice == GREENHOUSE_VENTILATE) {
      statusIcon = "🟡 WACHT / INGRIJPEN VEREIST";
      diagnoseWaarom = kasAdviceReason;
    }

    // 2. Wat moet JIJ doen (Actie)
    String actieVoorJij = "Alles loopt automatisch. Geen handmatige actie nodig.";
    if (isKasSleeping) {
      actieVoorJij = "Niets, de kas is in slaapstand.";
    } else if (millis() < SYSTEM_STARTUP_DELAY) {
      actieVoorJij = "Even geduld: Systeem kalibreert nog.";
    } else if (moldRisk) {
      if (outdoorHumidity > 85.0) {
        actieVoorJij = "🔴 Schimmelrisico, MAAR buiten/woning is te klam (>85% LV). **Houd ramen dicht!**";
      } else {
        actieVoorJij = "🏠🪟 **Jouw actie vereist:** Zet de binnendeur op een kier of activeer de woningventilatie om drogere lucht aan te voeren.";
      }
    }

    // 3. Hardware & Systeemstatus
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
      (millis() < SYSTEM_STARTUP_DELAY ? "⏳ Kalibreren" : "🟢 Operationeel")
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
  else if (text == "sleep" || text == "kasslaap" || text == "kweek") {
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
    
    float failKas = (totalKasDhtReads > 0) ? ((float)failedKasDhtReads / totalKasDhtReads) * 100.0 : 0.0;
    float failIndoor = (totalIndoorDhtReads > 0) ? ((float)failedIndoorDhtReads / totalIndoorDhtReads) * 100.0 : 0.0;

    int fanIntPct  = map(fanIntSpeed, 0, 255, 0, 100);
    int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
    int fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);
    
    float espInternalTemp = readEspInternalTemp();

    char hcBuf[1024];
    snprintf(hcBuf, sizeof(hcBuf),
      "🛠️ *Hardware & Sensor Check*\n\n"
      "⚙️ *Systeem:*\n"
      "• RAM: `%.1f KB` vrije ruimte\n"
      "• ESP32 Chip-Temp: `%.1f°C` 🧠\n\n"
      "📊 *Sensoren:*\n"
      "• Lichtsterkte: `%.0f lx`\n"
      "• Kas (DHT): Temp `%s` | LV `%s` (Fouten: `%.1f%%`)\n"
      "• Woning (BMP180): Temp `%s` | `%.2f hPa`\n"
      "• Woning (DHT LV): `%s` (Fouten: `%.1f%%`)\n\n"
      "🌀 *Fans (2 Extern + Intern):*\n"
      "• Intern (Circulatie): `%d%%` (`%d RPM`)\n"
      "• Extern 1 (Hoofd): `%d%%` (`%d RPM`)\n"
      "• Extern 2 (Boost): `%d%%` (`%d RPM`)\n",
      heapKb,
      espInternalTemp,
      currentLuxValue,
      formatVal(displayedTemp, 1, "°C").c_str(), formatVal(kasSmoothedHum, 0, "%").c_str(), failKas,
      formatVal(indoorSmoothedTemp, 1, "°C").c_str(), currentPressure,
      formatVal(indoorHum, 0, "%").c_str(), failIndoor,
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
  telegramMsg = "🚀 *De kleine kas (ESP32-S3) is opgestart*\n";
  telegramMsg += "⚙️ *Reden:* " + bootReason + "\n";
  telegramMsg += "📶 *SSID:* " + currentSSID + "\n";
  telegramMsg += "🌐 *IP:* `" + currentIp + "`";

  if (bot.sendMessage(telid, telegramMsg, "Markdown")) {
    logToSyslogAndSerial(F("[TELEGRAM] Opstartmelding succesvol verzonden."));
  }
}