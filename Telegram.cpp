/**
 * @file Telegram.cpp
 * @brief Geoptimaliseerde implementatie van Telegram bot interactie met veilige rate-limiting en non-blocking achtergrondcheck voor ESP32-S3.
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

// BUFFER VOOR TELEGRAM RAPPORTEN
char advBuffer[1600];

/**
 * @brief Stuurt een bericht via de Telegram bot met ingebouwde veiligheid en minimale vertraging.
 */
void sendTelegramAlert(String message) {
  if (WiFi.status() != WL_CONNECTED) return;

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

String buildStatusReport() {
  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
  int fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

  bool anyFanActive = (fanExt1Pct > 0 || fanExt2Pct > 0 || fanIntPct > 0);

  // 1. Systeemstatus van de Kas (Wat regelt de ESP zelf?)
  String regelingStatus = "🟢 Kas-klimaat is optimaal en stabiel.";
  if (millis() < SYSTEM_STARTUP_DELAY) {
    regelingStatus = "⏳ Systeem is aan het opstarten / kalibreren.";
  } else if (kasVpd < VPD_MIN_OPTIMAL) {
    regelingStatus = moldRisk ? "⚠️ Schimmelrisico in kas (ESP stuurt bij)." : "⚠️ Kas aan de klamme kant (ESP regelt).";
  } else if (kasVpd > VPD_MAX_OPTIMAL) {
    regelingStatus = "🔥 Uitdrogingsrisico in kas (ESP houdt kas gesloten).";
  } else if (anyFanActive || isHeatMatRecommended) {
    regelingStatus = "🔵 ESP voert actieve kas-regulering uit.";
  }

  // 2. Wat doet de ESP automatisch in de kas?
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

  // 3. Toelichting (Waarom doet de ESP dit?)
  String toelichtingTekst = kasAdviceReason;
  if (toelichtingTekst.length() == 0) {
    toelichtingTekst = "Geen actie vereist; de kas draait volledig zelfstandig.";
  }

  // 4. Wat wordt er van JOU (de mens / woning-actuator) verwacht?
  // (Alleen als jij actie moet ondernemen tussen woning en buiten)
  String actieVereist = "🟢 Geen actie nodig. De ESP regelt de kas volledig zelf.";
  if (millis() < SYSTEM_STARTUP_DELAY) {
    actieVereist = "⏳ Even geduld: Systeem kalibreert nog.";
  } else if (houseAdvice == HOUSE_VENTILATE && moldRisk) {
    actieVereist = "🏠🪟 **Jouw actie vereist:** Zet het raam van de woning open (zodat de woning als buffer kan dienen).";
  }

  String currentIp = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "Niet verbonden";

  static char reportBuf[1600];
  
  int written = snprintf(reportBuf, sizeof(reportBuf),
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

String buildHelpMenu() {
  String helpMsg = "🤖 *Beschikbare commando's:*\n\n"
                   "• *Status* (`/st`) - Bekijk direct alle sensorwaarden\n"
                   "• *Advies* (`/ad`) - Uitgebreid ventilatie & schimmeladvies\n"
                   "• *Minmax* (`/mm`) - Hoogste/laagste dagtemperatuur\n"
                   "• *Testmat* (`/tm`) - Test warmtemat (15 sec)\n"
                   "• *Testschimmel* (`/ts`) - Test schimmeltoggle (10 min)\n"
                   "• *Testfans* (`/tf`) - Test alle fans op 100% (30 sec)\n"
                   "• *Hardwarecheck* (`/hc`) - Meetwaardes hardware\n"
                   "• *Start AP/OTA* (`/ota`) - Start Access Point voor OTA\n"
                  //  "• *Stop AP/OTA* (`/sota`) - Sluit Access Point handmatig\n"
                   "• *Flush* (`/fl`) - Wis Telegram wachtrij\n"
                   "• *Reboot* (`/rb`) - Herstart het systeem\n"
                   "• *Help* (`/h`) - Dit menu\n";
  return helpMsg;
}

void sendHelpMenu() {
  sendTelegramAlert(buildHelpMenu());
}


void handleTelegramIncoming() {
  if (millis() - lastBotCheckTime <= BOT_CHECK_INTERVAL) return;
  lastBotCheckTime = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  yield(); 
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1); 
  if (numNewMessages <= 0) return;
  yield(); 

  // Verwerk uitsluitend het meest recente bericht
  int lastIdx = numNewMessages - 1;

  // BELANGRIJK: Zet de offset van te voren direct goed! 
  // Hiermee voorkom je dat Telegram bij een herstart of crash hetzelfde commando oneindig blijft spammen.
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

    String statusIcon = "🟢 VEILIG KLIMAAT";
    if (millis() < SYSTEM_STARTUP_DELAY) {
      statusIcon = "⏳ 🔵 OPSTARTEN / KALIBREREN";
    } else if (houseAdvice == HOUSE_VENTILATE && moldRisk) {
      statusIcon = "🚨 🔴 ZET RAAM WONING OPEN!";
    } else if (moldRisk) {
      if (anyFanActive || kasAdvice != OFF) {
        statusIcon = "⚠️ 🟡 RISICO (ZELF-OPLOSSEND)";
      } else {
        statusIcon = "🚨 🔴 ACTIE VEREIST!";
      }
    } else if (houseAdvice == HOUSE_VENTILATE) {
      statusIcon = "🏠 VENTILEER DE WONING";
    } else if (isHeatMatRecommended) {
      statusIcon = "🔥 WARMTEMAT AANBEVOLEN";
    }

    String kasHumDisplay = (kasSmoothedHum > HUM_MOLD_THRESHOLD) ? "🔴 `" + String(kasSmoothedHum, 1) + "%` (Te hoog)" : "`" + String(kasSmoothedHum, 1) + "%`";
    String schimmelRedenDisplay = moldRisk ? "🔴 _" + moldReasonText + "_" : "_" + moldReasonText + "_";

    static char advBufferLocal[1400];
    snprintf(advBufferLocal, sizeof(advBufferLocal),
      "📊 *Klimaat & Schimmeldiagnose*\n"
      "%s\n\n"
      "🪴 *KAS WAARDEN*\n"
      "• LV: %s (Drempel: >`%.1f%%`)\n"
      "• Dauwpunt-marge: `%+.1f °C` %s\n"
      "• VPD: `%+.2f kPa`%s\n"
      "• Schimmelreden: %s\n\n"
      "🏡 *WONING*\n"
      "• VPD: `%+.2f kPa` %s\n"
      "• Dauwpunt-marge: `%+.1f °C` %s\n\n"
      "⚙️️ *ACTIE & REGELING*\n"
      "• Woning ventileren: %s\n"
      "• Kas ventileren: %s\n"
      "• Verwarmingsmat: %s\n"
      "• Schimmelrisico: `%s`\n\n"
      "🌀 *VENTILATORS (2 EXTERN + INTERN)*\n"
      "• Intern: `%d%%` (`%d RPM`)\n"
      "• Ext 1 (Hoofd): `%d%%` (`%d RPM`)\n"
      "• Ext 2 (Boost): `%d%%` (`%d RPM`)",
      statusIcon.c_str(),
      kasHumDisplay.c_str(), HUM_MOLD_THRESHOLD,
      kasDpMargin, kasDpIcon.c_str(),
      kasVpd, vpdStatusText.c_str(),
      schimmelRedenDisplay.c_str(),
      indoorVpd, indoorVpdIcon.c_str(),
      indoorDpMargin, indoorDpIcon.c_str(),
      (houseAdvice == HOUSE_VENTILATE ? "JA ➡️" : "NEE 🔒"),
      (kasAdvice == GREENHOUSE_VENTILATE ? "Naar kamer 🔄" : (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL ? "Intern 🔄" : "Geen actie ✅")),
      (isHeatMatRecommended ? "❗ AAN 🔥" : "💤 UIT"),
      (moldRisk ? "JA 🔴" : "NEE 🟢"),
      fanIntPct, fanIntRPM,
      fanExt1Pct, fanExt1RPM,
      fanExt2Pct, fanExt2RPM
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
  else if (text == "reboot" || text == "rb") {
    // 1. Stuur de melding
    bot.sendMessage(telid, "⚠ Systeem wordt herstart...", "");
    logToSyslogAndSerial(F("[TELEGRAM] Handmatige reboot aangevraagd. Wachtrij opschonen..."));

    // 2. Markeer het bericht definitief als gelezen op de Telegram server
    // Door de offset op te hogen naar messages[lastIdx].message_id + 1 haalt hij dit bericht nooit meer op
    // bot.last_message_received = bot.messages[lastIdx].message_id;
    bot.last_message_received = bot.messages[lastIdx].update_id;
    // 3. Forceer een lege getUpdates update om de offset op de Telegram server te synchroniseren
    // Dit vertelt de Telegram server: "Ik heb al mijn berichten tot dit ID gelezen"
    bot.getUpdates(bot.last_message_received + 1);

    // 4. Geef de netwerk-stack tijd om de verbinding netjes te verbreken
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
    
    char hcBuf[850];
    snprintf(hcBuf, sizeof(hcBuf),
      "🛠️ *Hardware & Sensor Check*\n\n"
      "⚙️ *Systeem:*\n"
      "• RAM: `%.1f KB` vrije ruimte\n\n"
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
  else if (text == "ota") {
    // Bouw het bericht dynamisch op met de echte gegevens uit Config.h
    String apMsg = "🚀 *Access Point wordt gestart...*\n\n";
    apMsg += "• *SSID:* `" + String(AP_SSID) + "`\n";
    apMsg += "• *Wachtwoord:* `" + String(AP_PASS) + "`\n\n";
    apMsg += "Verbind je PC hiermee en stuur de OTA update.";

    // Markeer het bericht als gelezen zodat het niet herhaald wordt na reboot
    bot.last_message_received = bot.messages[lastIdx].update_id;
    bot.getUpdates(bot.last_message_received + 1);

    // Stuur het bericht nog even snel naar Telegram (lukt nét voor de wifiverbinding wegvalt)
    bot.sendMessage(senderChatId, apMsg, "Markdown");

    // Start het Access Point
    startAPMode();
  }
  // Telegram (nog) werkt niet in AP mode
  // else if (text == "sota") {
  //   bot.sendMessage(senderChatId, "Access Point wordt gesloten, verbind weer met normale netwerk...", "");
  //   stopAPMode();
  // }
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