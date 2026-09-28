/**
 * @file Mqtt.cpp
 * @brief Implementatie van MQTT-communicatie, offline buffering via LittleFS en Home Assistant integratie voor ESP32-S3.
 */

#include "nono.h"
#include "MQTT.h"
#include "Config.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

/**
 * @brief Initialiseert de MQTT client en stelt de server in.
 */
void setupMqtt() {
  mqttClient.setServer(SECRET_MQTT_SERVER_IP, mqtt_port); 
  mqttClient.setBufferSize(1024); // ⚠️ KRITIEK: Vergroot de buffer zodat de grote JSON (850b) past!
}
/**
 * @brief Beheert de MQTT-verbinding en periodieke verzending in de loop.
 */
void handleMqtt() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      reconnectMqtt();
    }
    mqttClient.loop();

    // Verstuur data elke 30 seconden
    static unsigned long lastMqttSend = 0;
    if (millis() - lastMqttSend >= 30000) {
      lastMqttSend = millis();
      sendMqttData();
    }
  }
}

/**
 * @brief Slaat een mislukt MQTT-bericht op in het LittleFS-bufferbestand.
 */
void savePayloadToBuffer(String payload) {
  File checkFile = LittleFS.open(BUFFER_FILE, "r");
  int lineCount = 0;
  if (checkFile) {
    while (checkFile.available()) {
      if (checkFile.read() == '\n') lineCount++;
    }
    checkFile.close();
  }

  if (lineCount >= MAX_BUFFER_SIZE) return;

  File file = LittleFS.open(BUFFER_FILE, "a");
  if (file) {
    file.println(payload);
    file.close();
  }
}

/**
 * @brief Stuurt opgeslagen berichten uit de buffer alsnog naar de MQTT-broker.
 */
void flushBufferToMqtt() {
  if (!LittleFS.exists(BUFFER_FILE)) return;
  File file = LittleFS.open(BUFFER_FILE, "r");
  if (!file) return;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0 && mqttClient.connected()) {
      mqttClient.publish(mqtt_topic, line.c_str());
      delay(50); 
    }
  }
  file.close();
  LittleFS.remove(BUFFER_FILE); 
}

/**
 * @brief Herkoppel met de MQTT-broker indien verbroken.
 */
void reconnectMqtt() {
  if (!mqttClient.connected()) {
    uint64_t chipId = ESP.getEfuseMac();
    String clientId = "Esp32S3Kas-";
    clientId += String((uint32_t)(chipId >> 32), HEX);
    
    if (mqttClient.connect(clientId.c_str(), mqtt_status_topic, 1, true, "offline")) {
      mqttClient.publish(mqtt_status_topic, "online", true); 
      flushBufferToMqtt(); 
    }
  }
}

/**
 * @brief Verzamelt alle meetwaarden en publiceert deze via MQTT (of buffert ze bij storing).
 */
void sendMqttData() {
  if (millis() < 5000) {
    return; 
  }

  JsonDocument doc;
  
  // Kas data
  if (kasSmoothedTemp != -999.0) doc["kas_temp"] = kasSmoothedTemp;
  if (kasSmoothedHum != -999.0) doc["kas_vochtigheid"] = kasSmoothedHum;
  if (kasDewPoint != -999.0) doc["kas_dauwpunt"] = kasDewPoint;
  if (kasVpd != -999.0) doc["kas_vpd"] = kasVpd;
  
  // Woning sensor data
  if (indoorSmoothedTemp != -999.0) doc["binnen_temp"] = indoorSmoothedTemp;
  if (indoorSmoothedHum != -999.0) doc["binnen_vochtigheid"] = indoorSmoothedHum;
  if (indoorDewPoint != -999.0) doc["binnen_dauwpunt"] = indoorDewPoint;
  if (indoorVpd != -999.0) doc["binnen_vpd"] = indoorVpd;

  // Buiten data
  if (outdoorTemp != -999.0) doc["buiten_temp"] = outdoorTemp;
  if (outdoorHumidity != -999.0) doc["buiten_vochtigheid"] = outdoorHumidity;
  if (outdoorDewPoint != -999.0) doc["buiten_dauwpunt"] = outdoorDewPoint;
  if (outdoorVpd != -999.0) doc["buiten_vpd"] = outdoorVpd;
  
  // Overige data
  if (currentPressure > 0.0) doc["luchtdruk"] = currentPressure;
  doc["luchtdruk_trend"]    = baroTrendArrow;
  if (currentLuxValue >= 0.0) doc["licht_lux"] = currentLuxValue;
  doc["schimmel_risico"]    = moldRisk;
  doc["verwarmingsmat_aan"] = isHeatMatRecommended;
  
  // 🌀 Ventilator data (Alle 3 correct gekoppeld!)
  doc["fan_int_rpm"]        = fanIntRPM;
  doc["fan_int_pct"]        = map(fanIntSpeed, 0, 255, 0, 100);
  
  doc["fan_ext1_rpm"]       = fanExt1RPM;
  doc["fan_ext1_pct"]       = map(fanExt1Speed, 0, 255, 0, 100);
  
  doc["fan_ext2_rpm"]       = fanExt2RPM;
  doc["fan_ext2_pct"]       = map(fanExt2Speed, 0, 255, 0, 100);

  doc["wifi_rssi"]          = wifiRSSI;
  doc["free_heap"]          = freeHeap;

  char buffer[850];
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
// /**
//  * @brief Initialiseert de MQTT client en stelt de server in.
//  */
// void setupMqtt() {
//   mqttClient.setServer(SECRET_MQTT_SERVER_IP, mqtt_port); //mqtt_server
// }

// /**
//  * @brief Beheert de MQTT-verbinding en periodieke verzending in de loop.
//  */
// void handleMqtt() {
//   if (WiFi.status() == WL_CONNECTED) {
//     if (!mqttClient.connected()) {
//       reconnectMqtt();
//     }
//     mqttClient.loop();

//     // Verstuur data op vaste tijden (bijv. elke 30 seconden)
//     static unsigned long lastMqttSend = 0;
//     if (millis() - lastMqttSend >= 30000) {
//       lastMqttSend = millis();
//       sendMqttData();
//     }
//   }
// }

// /**
//  * @brief Slaat een mislukt MQTT-bericht op in het LittleFS-bufferbestand.
//  */
// void savePayloadToBuffer(String payload) {
//   File checkFile = LittleFS.open(BUFFER_FILE, "r");
//   int lineCount = 0;
//   if (checkFile) {
//     while (checkFile.available()) {
//       if (checkFile.read() == '\n') lineCount++;
//     }
//     checkFile.close();
//   }

//   if (lineCount >= MAX_BUFFER_SIZE) return;

//   File file = LittleFS.open(BUFFER_FILE, "a");
//   if (file) {
//     file.println(payload);
//     file.close();
//   }
// }

// /**
//  * @brief Stuurt opgeslagen berichten uit de buffer alsnog naar de MQTT-broker.
//  */
// void flushBufferToMqtt() {
//   if (!LittleFS.exists(BUFFER_FILE)) return;
//   File file = LittleFS.open(BUFFER_FILE, "r");
//   if (!file) return;

//   while (file.available()) {
//     String line = file.readStringUntil('\n');
//     line.trim();
//     if (line.length() > 0 && mqttClient.connected()) {
//       mqttClient.publish(mqtt_topic, line.c_str());
//       delay(50); 
//     }
//   }
//   file.close();
//   LittleFS.remove(BUFFER_FILE); 
// }

// /**
//  * @brief Herkoppel met de MQTT-broker indien verbroken.
//  */
// void reconnectMqtt() {
//   if (!mqttClient.connected()) {
//     // Genereer unieke Client ID op basis van ESP32-S3 MAC adres
//     uint64_t chipId = ESP.getEfuseMac();
//     String clientId = "Esp32S3Kas-";
//     clientId += String((uint32_t)(chipId >> 32), HEX);
    
//     if (mqttClient.connect(clientId.c_str(), mqtt_status_topic, 1, true, "offline")) {
//       mqttClient.publish(mqtt_status_topic, "online", true); 
//       flushBufferToMqtt(); 
//     }
//   }
// }

// /**
//  * @brief Verzamelt alle meetwaarden en publiceert deze via MQTT (of buffert ze bij storing).
//  */
// void sendMqttData() {
//   if (millis() < 5000) {
//     return; 
//   }

//   JsonDocument doc;
  
//   // Kas data
//   if (kasSmoothedTemp != -999.0) doc["kas_temp"] = kasSmoothedTemp;
//   if (kasSmoothedHum != -999.0) doc["kas_vochtigheid"] = kasSmoothedHum;
//   if (kasDewPoint != -999.0) doc["kas_dauwpunt"] = kasDewPoint;
//   if (kasVpd != -999.0) doc["kas_vpd"] = kasVpd;
  
//   // Woning sensor data
//   if (indoorSmoothedTemp != -999.0) doc["binnen_temp"] = indoorSmoothedTemp;
//   if (indoorSmoothedHum != -999.0) doc["binnen_vochtigheid"] = indoorSmoothedHum;
//   if (indoorDewPoint != -999.0) doc["binnen_dauwpunt"] = indoorDewPoint;
//   if (indoorVpd != -999.0) doc["binnen_vpd"] = indoorVpd;

//   // Buiten data
//   if (outdoorTemp != -999.0) doc["buiten_temp"] = outdoorTemp;
//   if (outdoorHumidity != -999.0) doc["buiten_vochtigheid"] = outdoorHumidity;
//   if (outdoorDewPoint != -999.0) doc["buiten_dauwpunt"] = outdoorDewPoint;
//   if (outdoorVpd != -999.0) doc["buiten_vpd"] = outdoorVpd;
  
//   // Overige data
//   if (currentPressure > 0.0) doc["luchtdruk"] = currentPressure;
//   doc["luchtdruk_trend"]    = baroTrendArrow;
//   if (currentLuxValue >= 0.0) doc["licht_lux"] = currentLuxValue;
//   doc["schimmel_risico"]    = moldRisk;
//   doc["verwarmingsmat_aan"] = isHeatMatRecommended;
  
//   // Ventilator data
//   doc["fan_int_rpm"]        = fanExt1RPM;
//   doc["fan_ext_rpm"]        = fanExt2RPM;
//   doc["fan_int_pct"]        = map(fanExt1Speed, 0, PWM_RANGE, 0, 100);
//   doc["fan_ext_pct"]        = map(fanExt2Speed, 0, PWM_RANGE, 0, 100);

//   doc["wifi_rssi"]          = wifiRSSI;
//   doc["free_heap"]          = freeHeap;

//   char buffer[768];
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