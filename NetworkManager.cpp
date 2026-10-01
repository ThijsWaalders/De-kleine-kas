/**
 * @file NetworkManager.cpp
 * @brief Implementatie van Wi-Fi herverbinding, RTC crash-logging, Dedicated Core 0 OTA,
 *        Telegram-triggered AP-modus en Telnet-server voor ESP32-S3.
 */

#include "Config.h"
#include "NetworkManager.h"
#include "Logger.h"
#include "MQTT.h"
#include <ArduinoOTA.h>

IPAddress staticIP(SECRET_IP);
IPAddress gateway(GATEWAY);
IPAddress subnet(SUBNET);
IPAddress primaryDNS(PRIMARY_DNS);
IPAddress secondaryDNS(SECONDARY_DNS);

bool apModeActive = false;
unsigned long apStartTime = 0;
const unsigned long AP_TIMEOUT_MS = 5 * 60 * 1000; // 5 minuten timeout
bool apTimedOut = false; // Houdt bij of we door een timeout zijn herstart
bool otaInProgress = false;

// --- TELNET CONFIGURATIE ---
WiFiServer telnetServer(23);
WiFiClient telnetClient;

/**
 * @brief Print een string naar Serial en actieve Telnet-clients zonder regeleinde.
 */
void logPrint(const String &msg) {
  Serial.print(msg);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(msg);
  }
}

/**
 * @brief Print een string naar Serial en actieve Telnet-clients mét regeleinde.
 */
void logPrintln(const String &msg) {
  Serial.println(msg);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.println(msg);
  }
}

/**
 * @brief Print flash-string (F-macro) naar Serial en Telnet zonder regeleinde.
 */
void logPrint(const __FlashStringHelper *msg) {
  Serial.print(msg);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(msg);
  }
}

/**
 * @brief Print flash-string (F-macro) naar Serial en Telnet mét regeleinde.
 */
void logPrintln(const __FlashStringHelper *msg) {
  Serial.println(msg);
  if (telnetClient && telnetClient.connected()) {
    telnetClient.println(msg);
  }
}

/**
 * @brief Achtergrondtaak die exclusief draait op Core 0 voor onverwoestbare OTA updates.
 * Werkt zowel in Station-modus als in AP-modus.
 */
void otaBackgroundCode(void * pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED || apModeActive) {
      ArduinoOTA.handle();
    }
    vTaskDelay(pdMS_TO_TICKS(25)); // Kleine pauze zodat Core 0 niet 100% belast wordt
  }
}

/**
 * @brief Start de dedicated OTA-taak op Core 0.
 */
void setupOtaTask() {
  xTaskCreatePinnedToCore(
    otaBackgroundCode,   
    "OtaTask",           
    4096,                
    NULL,                
    3,                   
    NULL,                
    0                    // Pin op Core 0
  );
}

/**
 * @brief Start het Access Point met een stabiele DHCP-server en actieve OTA.
 */
void startAPMode() {
  logToSyslogAndSerial(F("[AP MODE] Schakelt over naar Access Point modus..."));
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);

  WiFi.mode(WIFI_AP);
  delay(100);

  IPAddress apIP(192, 168, 2, 1);
  IPAddress apGateway(192, 168, 2, 1);
  IPAddress apSubnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);

  bool success = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  
  if (success) {
    apModeActive = true;
    apStartTime = millis(); 
    apTimedOut = false;     

    ArduinoOTA.end();
    ArduinoOTA.setHostname("ESP32-S3-Zero-1");
    ArduinoOTA.begin();
    // Voeg dit toe waar je ArduinoOTA instelt (zowel in setupNetwork als in startAPMode):
    ArduinoOTA.onStart([]() {
      otaInProgress = true;
      logToSyslogAndSerial(F("[OTA] Update gestart! AP-timeout wordt tijdelijk gepauzeerd."));
    });
    
    ArduinoOTA.onEnd([]() {
      otaInProgress = false;
      logToSyslogAndSerial(F("[OTA] Update voltooid!"));
    });

    ArduinoOTA.onError([](ota_error_t error) {
      otaInProgress = false;
      logToSyslogAndSerialPrintf("[OTA ERROR] Foutcode: %u", error);
    });

    logToSyslogAndSerialPrintf("[AP MODE] Succes! SSID: %s | IP: %s", AP_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    logToSyslogAndSerial(F("[AP MODE ERROR] Kon Access Point niet starten!"));
  }
}

/**
 * @brief Stopt het AP handmatig.
 */
void stopAPMode() {
  logToSyslogAndSerial(F("[AP MODE] Sluit Access Point af, terug naar Station modus..."));
  WiFi.softAPdisconnect(true);
  apModeActive = false;
  apStartTime = 0;
  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_SSID, SECRET_PASS);
}

/**
 * @brief Hulpfunctie om Wi-Fi te starten met een vast IP-adres voor de ESP32-S3.
 */
void beginWithStaticIP(const char* targetSsid, const char* targetPassword) {
  if (apModeActive) return;
  WiFi.config(staticIP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(targetSsid, targetPassword);
  WiFi.setSleep(false);
}

/**
 * @brief Logt en analyseert de netwerkstatus na een herstart.
 */
void handleRtcConnectionLogging() {
  esp_reset_reason_t reason = esp_reset_reason();
  String bootReason = "Onbekend";
  
  switch (reason) {
    case ESP_RST_POWERON:   bootReason = "Power On"; break;
    case ESP_RST_EXT:       bootReason = "External Pin"; break;
    case ESP_RST_SW:        bootReason = "Software Reset"; break;
    case ESP_RST_PANIC:     bootReason = "Exception / Panic"; break;
    case ESP_RST_INT_WDT:   bootReason = "Internal WDT"; break;
    case ESP_RST_TASK_WDT:  bootReason = "Task WDT"; break;
    case ESP_RST_DEEPSLEEP: bootReason = "Deep Sleep Wake"; break;
    case ESP_RST_BROWNOUT:  bootReason = "Brownout Reset"; break;
    default:                bootReason = "Overig"; break;
  }

  String currentSSID  = WiFi.SSID();
  long signalRSSI     = WiFi.RSSI();

  logToSyslogAndSerialPrintf("[AP CHECK] Boot reden: %s | Verbonden met: %s | RSSI: %d dBm", 
                             bootReason.c_str(), currentSSID.c_str(), signalRSSI);

  String debugMsg = "🌡️ *Weerstation (ESP32-S3) Opstart Analyse*\n";
  debugMsg += "▶️ *Boot reden:* " + bootReason + "\n";
  debugMsg += "▶️ *Verbonden op SSID:* " + currentSSID + "\n";
  debugMsg += "📊 *Signaalsterkte:* " + String(signalRSSI) + " dBm";

  if (mqttClient.connected()) {
    mqttClient.publish("weerstation/debug_ap_switch", debugMsg.c_str(), true);
  }
}

/**
 * @brief Non-blocking state machine voor het bewaken en herstellen van de Wi-Fi verbinding.
 */
void handleWiFiReconnect() {
  if (apModeActive) {
    return;
  }

  static WifiReconnectState wifiReconnectState = WIFI_IDLE;
  static unsigned long actionTimer = 0;

  if (WiFi.status() == WL_CONNECTED) {
    if (hadWifiDrop || wifiReconnectState != WIFI_IDLE) {
      hadWifiDrop = false;
      wifiTotalDropStartTime = 0;
      wifiReconnectState = WIFI_IDLE;
    }
    return; 
  }

  if (wifiTotalDropStartTime == 0) {
    wifiTotalDropStartTime = millis();
    hadWifiDrop = true;
    wifiReconnectState = WIFI_WAITING_DROP_GRACE;
    actionTimer = millis();
  }

  if (millis() - wifiTotalDropStartTime > 180000) {
    logToSyslogAndSerial(F("[WIFI CRITICAL] Geen verbinding na 3 minuten, systeem herstart..."));
    delay(100);
    ESP.restart();
  }

  switch (wifiReconnectState) {
    case WIFI_WAITING_DROP_GRACE:
      if (millis() - actionTimer > 10000) {
        logToSyslogAndSerial(F("[WIFI] Verbinding verloren, herverbinden..."));
        beginWithStaticIP(SECRET_SSID, SECRET_PASS);
        wifiReconnectState = WIFI_TRYING_SSID1;
        actionTimer = millis();
      }
      break;

    case WIFI_TRYING_SSID1:
      if (millis() - actionTimer > 30000) { 
        logToSyslogAndSerial(F("[WIFI] Herverbinding mislukt, opnieuw proberen..."));
        beginWithStaticIP(SECRET_SSID, SECRET_PASS);
        actionTimer = millis();
      }
      break;

    default:
      wifiReconnectState = WIFI_WAITING_DROP_GRACE;
      break;
  }
}

/**
 * @brief Controleer in de loop of het AP te lang aan staat.
 */
void handleAPTimeout() {
  if (apModeActive) {
    // Als er een OTA bezig is, pauzeren we de timeout klok door de starttijd steeds te verschuiven
    if (otaInProgress) {
      apStartTime = millis(); 
      return;
    }

    if (millis() - apStartTime > AP_TIMEOUT_MS) {
      logToSyslogAndSerial(F("[AP TIMEOUT] AP stond te lang aan zonder actie. Systeem herstart..."));
      apTimedOut = true; 
      delay(100);
      ESP.restart();
    }
  }
}
// void handleAPTimeout() {
//   if (apModeActive) {
//     if (millis() - apStartTime > AP_TIMEOUT_MS) {
//       logToSyslogAndSerial(F("[AP TIMEOUT] AP stond te lang aan zonder actie. Systeem herstart..."));
//       apTimedOut = true; 
//       delay(100);
//       ESP.restart();
//     }
//   }
// }

/**
 * @brief Initialiseert het netwerk bij opstarten en start de OTA achtergrondtaak.
 */
void setupNetwork() {
  logToSyslogAndSerial("[NETWERK] Verbinden met Wi-Fi (statisch IP)...");
  
  WiFi.mode(WIFI_STA);
  WiFi.config(staticIP, gateway, subnet, primaryDNS, secondaryDNS);
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  WiFi.setSleep(false);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 6000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    logToSyslogAndSerial("[NETWERK] Verbonden met vast IP-adres: " + WiFi.localIP().toString());
  } else {
    logToSyslogAndSerial("[NETWERK WARNING] Kon niet direct verbinden binnen 6 seconden. Gaat door op achtergrond...");
  }

  // --- Telnet Server starten ---
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  logPrintln(F("[Telnet] Telnet server gestart op poort 23"));

  // // --- ArduinoOTA configureren ---
  // ArduinoOTA.setHostname("ESP32-S3-Zero-1");
  // ArduinoOTA.begin();
  // Voeg dit toe waar je ArduinoOTA instelt (zowel in setupNetwork als in startAPMode):
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    logToSyslogAndSerial(F("[OTA] Update gestart! AP-timeout wordt tijdelijk gepauzeerd."));
  });
  
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    logToSyslogAndSerial(F("[OTA] Update voltooid!"));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    logToSyslogAndSerialPrintf("[OTA ERROR] Foutcode: %u", error);
  });
  
  setupOtaTask();
  handleRtcConnectionLogging();
}

/**
 * @brief Hoofdlus om de netwerkstatus, timeout en inkomende Telnet-clients te beheren.
 */
void handleNetwork() {
  // Beheer inkomende Telnet-clients (beperk tot 1 actieve client tegelijk)
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) telnetClient.stop();
      telnetClient = telnetServer.available();
      logPrintln(F("[Telnet] Nieuwe verbinding geaccepteerd!"));
    } else {
      WiFiClient newClient = telnetServer.available();
      newClient.stop(); // Extra gelijktijdige verbindingen afwijzen
    }
  }

  handleWiFiReconnect();
  handleAPTimeout();
}
// /**
//  * @file NetworkManager.cpp
//  * @brief Implementatie van Wi-Fi herverbinding via een non-blocking state machine, RTC crash-logging,
//  *        Dedicated Core 0 OTA en Telegram-triggered AP-modus voor ESP32-S3.
//  */

// #include "Config.h"
// #include "NetworkManager.h"
// #include "Logger.h"
// #include "MQTT.h"
// #include <ArduinoOTA.h>

// IPAddress staticIP(SECRET_IP);
// IPAddress gateway(GATEWAY);
// IPAddress subnet(SUBNET);
// IPAddress primaryDNS(PRIMARY_DNS);
// IPAddress secondaryDNS(SECONDARY_DNS);

// bool apModeActive = false;
// unsigned long apStartTime = 0;
// const unsigned long AP_TIMEOUT_MS = 5 * 60 * 1000; // 5 minuten timeout
// bool apTimedOut = false; // Houdt bij of we door een timeout zijn herstart

// /**
//  * @brief Achtergrondtaak die exclusief draait op Core 0 voor onverwoestbare OTA updates.
//  * Werkt zowel in Station-modus als in AP-modus.
//  */
// void otaBackgroundCode(void * pvParameters) {
//   for (;;) {
//     if (WiFi.status() == WL_CONNECTED || apModeActive) {
//       ArduinoOTA.handle();
//     }
//     vTaskDelay(pdMS_TO_TICKS(25)); // Kleine pauze zodat Core 0 niet 100% belast wordt
//   }
// }

// /**
//  * @brief Start de dedicated OTA-taak op Core 0.
//  */
// void setupOtaTask() {
//   xTaskCreatePinnedToCore(
//     otaBackgroundCode,   
//     "OtaTask",           
//     4096,                
//     NULL,                
//     1,                   
//     NULL,                
//     0                    // Pin op Core 0
//   );
// }

// /**
//  * @brief Start het Access Point met een stabiele DHCP-server en actieve OTA.
//  */
// void startAPMode() {
//   logToSyslogAndSerial(F("[AP MODE] Schakelt over naar Access Point modus..."));
  
//   WiFi.disconnect(true);
//   WiFi.mode(WIFI_OFF);
//   delay(200);

//   WiFi.mode(WIFI_AP);
//   delay(100);

//   IPAddress apIP(192, 168, 2, 1);
//   IPAddress apGateway(192, 168, 2, 1);
//   IPAddress apSubnet(255, 255, 255, 0);
//   WiFi.softAPConfig(apIP, apGateway, apSubnet);

//   bool success = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
  
//   if (success) {
//     apModeActive = true;
//     apStartTime = millis(); 
//     apTimedOut = false;     

//     logToSyslogAndSerialPrintf("[AP MODE] Succes! SSID: %s | IP: %s", AP_SSID, WiFi.softAPIP().toString().c_str());
//   } else {
//     logToSyslogAndSerial(F("[AP MODE ERROR] Kon Access Point niet starten!"));
//   }
// }

// /**
//  * @brief Stopt het AP handmatig.
//  */
// void stopAPMode() {
//   logToSyslogAndSerial(F("[AP MODE] Sluit Access Point af, terug naar Station modus..."));
//   WiFi.softAPdisconnect(true);
//   apModeActive = false;
//   apStartTime = 0;
//   WiFi.mode(WIFI_STA);
//   WiFi.begin(SECRET_SSID, SECRET_PASS);
// }

// /**
//  * @brief Hulpfunctie om Wi-Fi te starten met een vast IP-adres voor de ESP32-S3.
//  */
// void beginWithStaticIP(const char* targetSsid, const char* targetPassword) {
//   if (apModeActive) return;
//   WiFi.config(staticIP, gateway, subnet, primaryDNS, secondaryDNS);
//   WiFi.begin(targetSsid, targetPassword);
//   WiFi.setSleep(false);
// }

// /**
//  * @brief Logt en analyseert de netwerkstatus na een herstart.
//  */
// void handleRtcConnectionLogging() {
//   esp_reset_reason_t reason = esp_reset_reason();
//   String bootReason = "Onbekend";
  
//   switch (reason) {
//     case ESP_RST_POWERON:   bootReason = "Power On"; break;
//     case ESP_RST_EXT:       bootReason = "External Pin"; break;
//     case ESP_RST_SW:        bootReason = "Software Reset"; break;
//     case ESP_RST_PANIC:     bootReason = "Exception / Panic"; break;
//     case ESP_RST_INT_WDT:   bootReason = "Internal WDT"; break;
//     case ESP_RST_TASK_WDT:  bootReason = "Task WDT"; break;
//     case ESP_RST_DEEPSLEEP: bootReason = "Deep Sleep Wake"; break;
//     case ESP_RST_BROWNOUT:  bootReason = "Brownout Reset"; break;
//     default:                bootReason = "Overig"; break;
//   }

//   String currentSSID  = WiFi.SSID();
//   long signalRSSI     = WiFi.RSSI();

//   logToSyslogAndSerialPrintf("[AP CHECK] Boot reden: %s | Verbonden met: %s | RSSI: %d dBm", 
//                              bootReason.c_str(), currentSSID.c_str(), signalRSSI);

//   String debugMsg = "🌡️ *Weerstation (ESP32-S3) Opstart Analyse*\n";
//   debugMsg += "▶️ *Boot reden:* " + bootReason + "\n";
//   debugMsg += "▶️ *Verbonden op SSID:* " + currentSSID + "\n";
//   debugMsg += "📊 *Signaalsterkte:* " + String(signalRSSI) + " dBm";

//   if (mqttClient.connected()) {
//     mqttClient.publish("weerstation/debug_ap_switch", debugMsg.c_str(), true);
//   }
// }

// /**
//  * @brief Non-blocking state machine voor het bewaken en herstellen van de Wi-Fi verbinding.
//  */
// void handleWiFiReconnect() {
//   if (apModeActive) {
//     return;
//   }

//   static WifiReconnectState wifiReconnectState = WIFI_IDLE;
//   static unsigned long actionTimer = 0;

//   if (WiFi.status() == WL_CONNECTED) {
//     if (hadWifiDrop || wifiReconnectState != WIFI_IDLE) {
//       hadWifiDrop = false;
//       wifiTotalDropStartTime = 0;
//       wifiReconnectState = WIFI_IDLE;
//     }
//     return; 
//   }

//   if (wifiTotalDropStartTime == 0) {
//     wifiTotalDropStartTime = millis();
//     hadWifiDrop = true;
//     wifiReconnectState = WIFI_WAITING_DROP_GRACE;
//     actionTimer = millis();
//   }

//   if (millis() - wifiTotalDropStartTime > 180000) {
//     logToSyslogAndSerial(F("[WIFI CRITICAL] Geen verbinding na 3 minuten, systeem herstart..."));
//     delay(100);
//     ESP.restart();
//   }

//   switch (wifiReconnectState) {
//     case WIFI_WAITING_DROP_GRACE:
//       if (millis() - actionTimer > 10000) {
//         logToSyslogAndSerial(F("[WIFI] Verbinding verloren, herverbinden..."));
//         beginWithStaticIP(SECRET_SSID, SECRET_PASS);
//         wifiReconnectState = WIFI_TRYING_SSID1;
//         actionTimer = millis();
//       }
//       break;

//     case WIFI_TRYING_SSID1:
//       if (millis() - actionTimer > 30000) { 
//         logToSyslogAndSerial(F("[WIFI] Herverbinding mislukt, opnieuw proberen..."));
//         beginWithStaticIP(SECRET_SSID, SECRET_PASS);
//         actionTimer = millis();
//       }
//       break;

//     default:
//       wifiReconnectState = WIFI_WAITING_DROP_GRACE;
//       break;
//   }
// }

// /**
//  * @brief Controleer in de loop of het AP te lang aan staat.
//  */
// void handleAPTimeout() {
//   if (apModeActive) {
//     if (millis() - apStartTime > AP_TIMEOUT_MS) {
//       logToSyslogAndSerial(F("[AP TIMEOUT] AP stond te lang aan zonder actie. Systeem herstart..."));
//       apTimedOut = true; 
//       delay(100);
//       ESP.restart();
//     }
//   }
// }

// /**
//  * @brief Initialiseert het netwerk bij opstarten en start de OTA achtergrondtaak.
//  */
// void setupNetwork() {
//   logToSyslogAndSerial("[NETWERK] Verbinden met Wi-Fi (statisch IP)...");
  
//   // 1. DIT ONTBRAK BIJ OPSTARTEN: Direct het vaste IP instellen voordat we verbinden
//   WiFi.mode(WIFI_STA);
//   WiFi.config(staticIP, gateway, subnet, primaryDNS, secondaryDNS);
//   WiFi.begin(SECRET_SSID, SECRET_PASS);
//   WiFi.setSleep(false);

//   unsigned long startAttemptTime = millis();
//   while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 6000) {
//     delay(500);
//     Serial.print(".");
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     logToSyslogAndSerial("[NETWERK] Verbonden met vast IP-adres: " + WiFi.localIP().toString());

//     // --- HIER MOET ARDUINOOTA.BEGIN() STAAN ---
//     ArduinoOTA.begin();
//   } else {
//     logToSyslogAndSerial("[NETWERK WARNING] Kon niet direct verbinden binnen 6 seconden. Gaat door op achtergrond...");
//   }

//   ArduinoOTA.setHostname("ESP32-S3-Zero-1");
//   ArduinoOTA.begin();
//   setupOtaTask();
//   handleRtcConnectionLogging();
// }

// /**
//  * @brief Hoofdlus om de netwerkstatus en timeout te bewaken.
//  */
// void handleNetwork() {
//   handleWiFiReconnect();
//   handleAPTimeout();
// }
