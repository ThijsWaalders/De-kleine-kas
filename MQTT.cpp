#include "MQTT.h"
#include "Config.h"
#include "ClimateLogic.h"
#include "KasSensors.h" // Nodig voor traySensor1Temp en traySensor2Temp
#include "Logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// extern const char* BUFFER_FILE;

// const char* BUFFER_FILE = "/mqtt_queue.json";
const int MAX_BUFFER_LINES = 50; // Bewaar max 50 meet-updates tijdens netwerkuitval

// --- HELPER: BEWAAR METING BIJ GEEN VERBINDING ---
void savePayloadToBuffer(const String& payload) {
  // Controleer eerst hoeveel regels er al inzitten om flash overbelasting te voorkomen
  File checkFile = LittleFS.open(BUFFER_FILE, "r");
  int lineCount = 0;
  if (checkFile) {
    while (checkFile.available()) {
      if (checkFile.read() == '\n') lineCount++;
    }
    checkFile.close();
  }

  if (lineCount >= MAX_BUFFER_LINES) {
    logToSyslogAndSerial("[MQTT BUFFER WARNING] Buffer vol (" + String(MAX_BUFFER_LINES) + " regels). Oude metingen worden niet overschreven.");
    return;
  }

  File file = LittleFS.open(BUFFER_FILE, "a");
  if (file) {
    file.println(payload);
    file.close();
    logToSyslogAndSerial("[MQTT BUFFER] Offline meting opgeslagen in LittleFS.");
  }
}

// --- NON-BLOCKING BUFFER FLUSH (PROFESSIONELE OPZET) ---
// Verwerkt exact 1 bericht per aanroep zolang de minimale tijdsinterval is verstreken.
void processBufferedMqtt() {
  if (!mqttClient.connected() || !LittleFS.exists(BUFFER_FILE)) return;

  static unsigned long lastBufferFlushTime = 0;
  // Pauzeer minimaal 50ms tussen het verzenden van gebufferde berichten
  if (millis() - lastBufferFlushTime < 50) return;

  File file = LittleFS.open(BUFFER_FILE, "r");
  if (!file || !file.available()) {
    if (file) file.close();
    LittleFS.remove(BUFFER_FILE); // Wis leeg bestand
    return;
  }

  // Lees het EERSTE bericht uit het bufferbestand
  String lineToSend = file.readStringUntil('\n');
  lineToSend.trim();

  // Lees de REST van het bestand in om straks terug te schrijven
  String remainingBuffer = "";
  while (file.available()) {
    remainingBuffer += file.readStringUntil('\n') + "\n";
  }
  file.close();

  // Probeer het eerste bericht te versturen naar Home Assistant / Broker
  if (lineToSend.length() > 0) {
    if (mqttClient.publish(mqtt_topic, lineToSend.c_str())) {
      lastBufferFlushTime = millis();
      logToSyslogAndSerial("[MQTT BUFFER] Gebufferd bericht succesvol ingehaald!");

      // Werk het bestand bij: schrijf alleen de resterende berichten terug
      if (remainingBuffer.length() > 0) {
        File outFile = LittleFS.open(BUFFER_FILE, "w");
        if (outFile) {
          outFile.print(remainingBuffer);
          outFile.close();
        }
      } else {
        LittleFS.remove(BUFFER_FILE); // Buffer is volledig leeg!
        logToSyslogAndSerial("[MQTT BUFFER] ✅ Alle gebufferde berichten succesvol ingehaald! Buffer gewist.");
      }
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Inkomende MQTT berichten verwerken indien gewenst
}

void setupMqtt() {
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(1536);
  mqttClient.setCallback(mqttCallback);
  logToSyslogAndSerial("[MQTT] MQTT client geconfigureerd.");
}

// Netwerk & verbinding onderhouden + buffer verwerken (NON-BLOCKING)
void handleMqtt() {
  if (WiFi.status() == WL_CONNECTED) {
    // Zorg dat de server en buffermaat altijd goed staan zodra WiFi actief is
    static bool mqttInitialized = false;
    if (!mqttInitialized) {
      mqttClient.setServer(mqtt_server, mqtt_port);
      mqttClient.setBufferSize(1536);
      mqttClient.setCallback(mqttCallback);
      mqttInitialized = true;
    }

    if (!mqttClient.connected()) {
      reconnectMqtt();
    }
    mqttClient.loop();

    // Verwerk gebufferde berichten (indien aanwezig)
    processBufferedMqtt();

    // Stuur elke 30 seconden nieuwe MQTT data naar de broker
    static unsigned long lastMqttSend = 0;
    if (millis() - lastMqttSend >= 30000) {
      lastMqttSend = millis();
      sendMqttData();
    }
  }
}

void reconnectMqtt() {
  if (!mqttClient.connected()) {
    uint64_t chipId = ESP.getEfuseMac();
    String clientId = "Esp32S3Kas-";
    clientId += String((uint32_t)(chipId >> 32), HEX);
    
    if (mqttClient.connect(clientId.c_str(), mqtt_status_topic, 1, true, "offline")) {
      mqttClient.publish(mqtt_status_topic, "online", true); 
      logToSyslogAndSerial("[MQTT] Opnieuw verbonden met MQTT broker.");
    }
  }
}

void sendMqttData() {
  if (millis() < 5000) return;

  JsonDocument doc;
  
  // --- KAS DATA (SHT31) ---
  if (kasSmoothedTemp != -999.0) doc["kas_temp"] = kasSmoothedTemp;
  if (kasSmoothedHum != -999.0) doc["kas_vochtigheid"] = kasSmoothedHum;
  if (kasDewPoint != -999.0) doc["kas_dauwpunt"] = kasDewPoint;
  if (kasVpd != -999.0) doc["kas_vpd"] = kasVpd;
  doc["kas_dp_marge"] = kasDpMargin;

  // --- TRAY SENSOREN (DS18B20) ---
  if (traySensor1Temp != -999.0) doc["tray_sensor_1_temp"] = traySensor1Temp;
  if (traySensor2Temp != -999.0) doc["tray_sensor_2_temp"] = traySensor2Temp;

  // --- WONING SENSOR DATA (BME280) ---
  if (indoorSmoothedTemp != -999.0) doc["binnen_temp"] = indoorSmoothedTemp;
  if (indoorSmoothedHum != -999.0) doc["binnen_vochtigheid"] = indoorSmoothedHum;
  if (indoorDewPoint != -999.0) doc["binnen_dauwpunt"] = indoorDewPoint;
  if (indoorVpd != -999.0) doc["binnen_vpd"] = indoorVpd;
  doc["binnen_dp_marge"] = indoorDpMargin;

  // --- BUITEN DATA ---
  if (outdoorTemp != -999.0) doc["buiten_temp"] = outdoorTemp;
  if (outdoorHumidity != -999.0) doc["buiten_vochtigheid"] = outdoorHumidity;
  if (outdoorDewPoint != -999.0) doc["buiten_dauwpunt"] = outdoorDewPoint;
  // Strikte grenscheck: Een realistische VPD ligt altijd tussen 0.0 en 5.0 kPa.
  // Alles daarbuiten (of -999.0) wordt genegeerd of als N/A behandeld.
  if (outdoorVpd != -999.0 && outdoorVpd >= 0.0 && outdoorVpd <= 5.0) {
    doc["buiten_vpd"] = outdoorVpd;
  }
  doc["buiten_dp_marge"] = outdoorDpMargin;
  
  // --- RISICO STATUSSEN ---
  doc["schimmel_risico"] = moldRisk;
  doc["uitdroging_risico"] = (kasVpd > VPD_MAX_OPTIMAL) ? 1 : 0;
  doc["te_klam_risico"] = (kasVpd < VPD_MIN_OPTIMAL || kasSmoothedHum >= HUM_MOLD_THRESHOLD) ? 1 : 0;
  doc["verwarmingsmat"] = isHeatMatRecommended ? 1 : 0;
  doc["testmodus_actief"] = isTestModeActive ? 1 : 0;

  // --- VENTILATIE ADVIEZEN ---
  doc["advies_kas_actief"] = (kasAdvice != KAS_OFF) ? 1 : 0;
  doc["advies_binnen_actief"] = (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) ? 1 : 0;
  doc["advies_buiten_actief"] = homeVentActive ? 1 : 0;  

  // --- SYSTEEM & KLIMAAT ---
  if (currentPressure > 0.0) doc["luchtdruk"] = currentPressure;
  doc["luchtdruk_trend"] = baroTrendArrow;
  if (currentLuxValue >= 0.0) doc["licht_lux"] = currentLuxValue;

  doc["schimmel_reden"] = moldReasonText;
  doc["kas_advies_reden"] = kasAdviceReason;
  doc["kas_vpd_status"] = vpdStatusText;
  doc["dp_marge_status"] = dpMarginStatusText;

  switch (kasAdvice) {
    case KAS_OFF: doc["kas_advies"] = "KAS_OFF"; break;
    case GREENHOUSE_VENTILATE: doc["kas_advies"] = "VENTILATE"; break;
    case GREENHOUSE_CIRCULATE_INTERNAL: doc["kas_advies"] = "CIRCULATE"; break;
    default: doc["kas_advies"] = "UNKNOWN"; break;
  }
  
  // --- VENTILATOR DATA ---
  doc["fan_int_rpm"] = fanIntRPM;
  doc["fan_int_pct"] = map(fanIntSpeed, 0, 255, 0, 100);
  doc["fan_ext1_rpm"] = fanExt1RPM;
  doc["fan_ext1_pct"] = map(fanExt1Speed, 0, 255, 0, 100);
  doc["fan_ext2_rpm"] = fanExt2RPM;
  doc["fan_ext2_pct"] = map(fanExt2Speed, 0, 255, 0, 100);

  // --- DIAGNOSTIEK & TELLERS ---
  doc["wifi_rssi"] = wifiRSSI;
  doc["free_heap"] = freeHeap;
  doc["failed_kas_reads"] = failedKasReads;
  doc["failed_indoor_reads"] = failedIndoorReads;

  float espInternalTemp = readEspInternalTemp();
  if (espInternalTemp != -999.0 && espInternalTemp != 0.0) {
    doc["esp_temp"] = espInternalTemp;
  }

  char buffer[1536];
  serializeJson(doc, buffer);

  if (mqttClient.connected()) {
    if (mqttClient.publish(mqtt_topic, buffer)) {
      lastSuccessfulNetworkActivity = millis(); 
    } else {
      savePayloadToBuffer(String(buffer));
    }
  } else {
    savePayloadToBuffer(String(buffer));
  }
}
// #include "MQTT.h"
// #include "Config.h"
// #include "ClimateLogic.h"
// #include "Logger.h"
// #include <LittleFS.h>
// #include <ArduinoJson.h>
// #include <WiFi.h>

// // extern const char* BUFFER_FILE;

// // const char* BUFFER_FILE = "/mqtt_queue.json";
// const int MAX_BUFFER_LINES = 50; // Bewaar max 50 meet-updates tijdens netwerkuitval

// // --- HELPER: BEWAAR METING BIJ GEEN VERBINDING ---
// void savePayloadToBuffer(const String& payload) {
//   // Controleer eerst hoeveel regels er al inzitten om flash overbelasting te voorkomen
//   File checkFile = LittleFS.open(BUFFER_FILE, "r");
//   int lineCount = 0;
//   if (checkFile) {
//     while (checkFile.available()) {
//       if (checkFile.read() == '\n') lineCount++;
//     }
//     checkFile.close();
//   }

//   if (lineCount >= MAX_BUFFER_LINES) {
//     logToSyslogAndSerial("[MQTT BUFFER WARNING] Buffer vol (" + String(MAX_BUFFER_LINES) + " regels). Oude metingen worden niet overschreven.");
//     return;
//   }

//   File file = LittleFS.open(BUFFER_FILE, "a");
//   if (file) {
//     file.println(payload);
//     file.close();
//     logToSyslogAndSerial("[MQTT BUFFER] Offline meting opgeslagen in LittleFS.");
//   }
// }

// // --- NON-BLOCKING BUFFER FLUSH (PROFESSIONELE OPZET) ---
// // Verwerkt exact 1 bericht per aanroep zolang de minimale tijdsinterval is verstreken.
// void processBufferedMqtt() {
//   if (!mqttClient.connected() || !LittleFS.exists(BUFFER_FILE)) return;

//   static unsigned long lastBufferFlushTime = 0;
//   // Pauzeer minimaal 50ms tussen het verzenden van gebufferde berichten
//   if (millis() - lastBufferFlushTime < 50) return;

//   File file = LittleFS.open(BUFFER_FILE, "r");
//   if (!file || !file.available()) {
//     if (file) file.close();
//     LittleFS.remove(BUFFER_FILE); // Wis leeg bestand
//     return;
//   }

//   // Lees het EERSTE bericht uit het bufferbestand
//   String lineToSend = file.readStringUntil('\n');
//   lineToSend.trim();

//   // Lees de REST van het bestand in om straks terug te schrijven
//   String remainingBuffer = "";
//   while (file.available()) {
//     remainingBuffer += file.readStringUntil('\n') + "\n";
//   }
//   file.close();

//   // Probeer het eerste bericht te versturen naar Home Assistant / Broker
//   if (lineToSend.length() > 0) {
//     if (mqttClient.publish(mqtt_topic, lineToSend.c_str())) {
//       lastBufferFlushTime = millis();
//       logToSyslogAndSerial("[MQTT BUFFER] Gebufferd bericht succesvol ingehaald!");

//       // Werk het bestand bij: schrijf alleen de resterende berichten terug
//       if (remainingBuffer.length() > 0) {
//         File outFile = LittleFS.open(BUFFER_FILE, "w");
//         if (outFile) {
//           outFile.print(remainingBuffer);
//           outFile.close();
//         }
//       } else {
//         LittleFS.remove(BUFFER_FILE); // Buffer is volledig leeg!
//         logToSyslogAndSerial("[MQTT BUFFER] ✅ Alle gebufferde berichten succesvol ingehaald! Buffer gewist.");
//       }
//     }
//   }
// }

// void mqttCallback(char* topic, byte* payload, unsigned int length) {
//   // Inkomende MQTT berichten verwerken indien gewenst
// }

// void setupMqtt() {
//   mqttClient.setServer(mqtt_server, mqtt_port);
//   mqttClient.setBufferSize(1536);
//   mqttClient.setCallback(mqttCallback);
//   logToSyslogAndSerial("[MQTT] MQTT client geconfigureerd.");
// }

// // Netwerk & verbinding onderhouden + buffer verwerken (NON-BLOCKING)
// void handleMqtt() {
//   if (WiFi.status() == WL_CONNECTED) {
//     // Zorg dat de server en buffermaat altijd goed staan zodra WiFi actief is
//     static bool mqttInitialized = false;
//     if (!mqttInitialized) {
//       mqttClient.setServer(mqtt_server, mqtt_port);
//       mqttClient.setBufferSize(1536);
//       mqttClient.setCallback(mqttCallback);
//       mqttInitialized = true;
//     }

//     if (!mqttClient.connected()) {
//       reconnectMqtt();
//     }
//     mqttClient.loop();

//     // Verwerk gebufferde berichten (indien aanwezig)
//     processBufferedMqtt();

//     // Stuur elke 30 seconden nieuwe MQTT data naar de broker
//     static unsigned long lastMqttSend = 0;
//     if (millis() - lastMqttSend >= 30000) {
//       lastMqttSend = millis();
//       sendMqttData();
//     }
//   }
// }

// void reconnectMqtt() {
//   if (!mqttClient.connected()) {
//     uint64_t chipId = ESP.getEfuseMac();
//     String clientId = "Esp32S3Kas-";
//     clientId += String((uint32_t)(chipId >> 32), HEX);
    
//     if (mqttClient.connect(clientId.c_str(), mqtt_status_topic, 1, true, "offline")) {
//       mqttClient.publish(mqtt_status_topic, "online", true); 
//       logToSyslogAndSerial("[MQTT] Opnieuw verbonden met MQTT broker.");
//     }
//   }
// }

// void sendMqttData() {
//   if (millis() < 5000) return;

//   JsonDocument doc;
  
//   // --- KAS DATA (SHT31) ---
//   if (kasSmoothedTemp != -999.0) doc["kas_temp"] = kasSmoothedTemp;
//   if (kasSmoothedHum != -999.0) doc["kas_vochtigheid"] = kasSmoothedHum;
//   if (kasDewPoint != -999.0) doc["kas_dauwpunt"] = kasDewPoint;
//   if (kasVpd != -999.0) doc["kas_vpd"] = kasVpd;
//   doc["kas_dp_marge"] = kasDpMargin;

//   // --- WONING SENSOR DATA (BME280) ---
//   if (indoorSmoothedTemp != -999.0) doc["binnen_temp"] = indoorSmoothedTemp;
//   if (indoorSmoothedHum != -999.0) doc["binnen_vochtigheid"] = indoorSmoothedHum;
//   if (indoorDewPoint != -999.0) doc["binnen_dauwpunt"] = indoorDewPoint;
//   if (indoorVpd != -999.0) doc["binnen_vpd"] = indoorVpd;
//   doc["binnen_dp_marge"] = indoorDpMargin;

//   // --- BUITEN DATA ---
//   if (outdoorTemp != -999.0) doc["buiten_temp"] = outdoorTemp;
//   if (outdoorHumidity != -999.0) doc["buiten_vochtigheid"] = outdoorHumidity;
//   if (outdoorDewPoint != -999.0) doc["buiten_dauwpunt"] = outdoorDewPoint;
  
//   // Strikte grenscheck: Een realistische VPD ligt altijd tussen 0.0 en 5.0 kPa.
//   // Alles daarbuiten (of -999.0) wordt genegeerd of als N/A behandeld.
//   if (outdoorVpd != -999.0 && outdoorVpd >= 0.0 && outdoorVpd <= 5.0) {
//     doc["buiten_vpd"] = outdoorVpd;
//   }
  
//   doc["buiten_dp_marge"] = outdoorDpMargin;
  
//   // // --- BUITEN DATA ---
//   // if (outdoorTemp != -999.0) doc["buiten_temp"] = outdoorTemp;
//   // if (outdoorHumidity != -999.0) doc["buiten_vochtigheid"] = outdoorHumidity;
//   // if (outdoorDewPoint != -999.0) doc["buiten_dauwpunt"] = outdoorDewPoint;
//   // if (outdoorVpd != -999.0) doc["buiten_vpd"] = outdoorVpd;
//   // doc["buiten_dp_marge"] = outdoorDpMargin;
  
//   // --- RISICO STATUSSEN ---
//   doc["schimmel_risico"] = moldRisk;
//   doc["uitdroging_risico"] = (kasVpd > VPD_MAX_OPTIMAL) ? 1 : 0;
//   doc["te_klam_risico"] = (kasVpd < VPD_MIN_OPTIMAL || kasSmoothedHum >= HUM_MOLD_THRESHOLD) ? 1 : 0;
//   doc["verwarmingsmat"] = isHeatMatRecommended ? 1 : 0;
//   doc["testmodus_actief"] = isTestModeActive ? 1 : 0;

//   doc["advies_kas_actief"] = (kasAdvice != KAS_OFF) ? 1 : 0;
//   doc["advies_binnen_actief"] = (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) ? 1 : 0;
//   doc["advies_buiten_actief"] = homeVentActive ? 1 : 0;  

//   // --- SYSTEEM & KLIMAAT ---
//   if (currentPressure > 0.0) doc["luchtdruk"] = currentPressure;
//   doc["luchtdruk_trend"] = baroTrendArrow;
//   if (currentLuxValue >= 0.0) doc["licht_lux"] = currentLuxValue;

//   doc["schimmel_reden"] = moldReasonText;
//   doc["kas_advies_reden"] = kasAdviceReason;
//   doc["kas_vpd_status"] = vpdStatusText;
//   doc["dp_marge_status"] = dpMarginStatusText;

//   switch (kasAdvice) {
//     case KAS_OFF: doc["kas_advies"] = "KAS_OFF"; break;
//     case GREENHOUSE_VENTILATE: doc["kas_advies"] = "VENTILATE"; break;
//     case GREENHOUSE_CIRCULATE_INTERNAL: doc["kas_advies"] = "CIRCULATE"; break;
//     default: doc["kas_advies"] = "UNKNOWN"; break;
//   }
  
//   // --- VENTILATOR DATA ---
//   doc["fan_int_rpm"] = fanIntRPM;
//   doc["fan_int_pct"] = map(fanIntSpeed, 0, 255, 0, 100);
//   doc["fan_ext1_rpm"] = fanExt1RPM;
//   doc["fan_ext1_pct"] = map(fanExt1Speed, 0, 255, 0, 100);
//   doc["fan_ext2_rpm"] = fanExt2RPM;
//   doc["fan_ext2_pct"] = map(fanExt2Speed, 0, 255, 0, 100);

//   // --- DIAGNOSTIEK & TELLERS ---
//   doc["wifi_rssi"] = wifiRSSI;
//   doc["free_heap"] = freeHeap;
//   doc["failed_kas_reads"] = failedKasReads;
//   doc["failed_indoor_reads"] = failedIndoorReads;

//   float espInternalTemp = readEspInternalTemp();
//   if (espInternalTemp != -999.0 && espInternalTemp != 0.0) {
//     doc["esp_temp"] = espInternalTemp;
//   }

//   char buffer[1536];
//   serializeJson(doc, buffer);

//   if (mqttClient.connected()) {
//     if (mqttClient.publish(mqtt_topic, buffer)) {
//       lastSuccessfulNetworkActivity = millis(); 
//     } else {
//       savePayloadToBuffer(String(buffer));
//     }
//   } else {
//     savePayloadToBuffer(String(buffer));
//   }
// }
