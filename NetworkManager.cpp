/**
 * @file NetworkManager.cpp
 * @brief Implementatie van Wi-Fi herverbinding via een non-blocking state machine, RTC crash-logging en Dedicated Core 0 OTA voor ESP32-S3.
 */

#include "Config.h"
#include "NetworkManager.h"
#include "Logger.h"
#include "MQTT.h"
#include <ArduinoOTA.h>

// Vaste netwerkinstellingen
IPAddress staticIP(192, 168, 4, 201);    // Vast IP voor deze ESP32
IPAddress gateway(192, 168, 4, 1);       // Gateway IP
IPAddress subnet(255, 255, 255, 0);      // Subnetmasker
IPAddress primaryDNS(192, 168, 1, 110);    // DNS1
IPAddress secondaryDNS(192, 168, 1, 120);   // DNS2

/**
 * @brief Achtergrondtaak die exclusief draait op Core 0 voor onverwoestbare OTA updates.
 */
void otaBackgroundCode(void * pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
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
    otaBackgroundCode,   // De taak-functie
    "OtaTask",           // Naam voor debug
    4096,                // Stack size in bytes
    NULL,                // Parameter
    1,                   // Prioriteit
    NULL,                // Task handle
    0                    // PIN OP CORE 0 (gescheiden van jouw loop op Core 1!)
  );
}

/**
 * @brief Hulpfunctie om Wi-Fi te starten met een vast IP-adres voor de ESP32-S3.
 */
void beginWithStaticIP(const char* targetSsid, const char* targetPassword) {
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
        beginWithStaticIP(ssid, password);
        wifiReconnectState = WIFI_TRYING_SSID1;
        actionTimer = millis();
      }
      break;

    case WIFI_TRYING_SSID1:
      if (millis() - actionTimer > 30000) { 
        logToSyslogAndSerial(F("[WIFI] Herverbinding mislukt, opnieuw proberen..."));
        beginWithStaticIP(ssid, password);
        actionTimer = millis();
      }
      break;

    default:
      wifiReconnectState = WIFI_WAITING_DROP_GRACE;
      break;
  }
}

/**
 * @brief Initialiseert het netwerk bij opstarten en start de OTA achtergrondtaak.
 */
void setupNetwork() {
  logToSyslogAndSerial(F("[NETWERK] Netwerk initialiseren..."));
  beginWithStaticIP(ssid, password);
  
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    logToSyslogAndSerial(F("\n========================================"));
    logToSyslogAndSerialPrintf("🌐 [NETWERK SUCCES] IP-ADRES: %s", WiFi.localIP().toString().c_str());
    logToSyslogAndSerial(F("========================================\n"));
  } else {
    logToSyslogAndSerial(F("[NETWERK WAARSCHUWING] Geen Wi-Fi verbinding binnen de tijd."));
  }

  // ArduinoOTA hostnaam en poort instellen
  ArduinoOTA.setHostname("ESP32-S3-Zero-1");
  ArduinoOTA.begin();

  // Start de onverwoestbare OTA taak op Core 0
  setupOtaTask();
  
  // Voer de RTC / AP check nu eenmalig uit
  handleRtcConnectionLogging();
}

// /**
//  * @brief Initialiseert het netwerk bij opstarten en start de OTA achtergrondtaak.
//  */
// void setupNetwork() {
//   logToSyslogAndSerial(F("[NETWERK] Netwerk initialiseren..."));
//   beginWithStaticIP(ssid, password);
  
//   unsigned long startAttempt = millis();
//   while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
//     delay(500);
//   }

//   // ArduinoOTA hostnaam en poort instellen
//   ArduinoOTA.setHostname("ESP32-S3-Zero-1");
//   ArduinoOTA.begin();

//   // Start de onverwoestbare OTA taak op Core 0
//   setupOtaTask();
  
//   handleRtcConnectionLogging();

//   // Wacht tot de verbinding definitief staat en print het duidelijk uit
//   int attempts = 0;
//   while (WiFi.status() != WL_CONNECTED && attempts < 20) {
//     delay(500);
//     attempts++;
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     logToSyslogAndSerial(F("\n========================================"));
//     logToSyslogAndSerialPrintf("🌐 [NETWERK SUCCES] IP-ADRES: %s", WiFi.localIP().toString().c_str());
//     logToSyslogAndSerial(F("========================================\n"));
//   } else {
//     logToSyslogAndSerial(F("[NETWERK WAARSCHUWING] Geen Wi-Fi verbinding binnen de tijd."));
//   }
// }

/**
 * @brief Hoofdlus om de netwerkstatus te bewaken.
 */
void handleNetwork() {
  handleWiFiReconnect();
}