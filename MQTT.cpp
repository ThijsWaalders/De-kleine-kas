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
 * @brief Callback functie voor inkomende MQTT berichten.
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Optioneel: verwerk hier inkomende berichten indien nodig
}

/**
 * @brief Initialiseert de MQTT client en stelt de server en buffer in.
 */
void setupMqtt() {
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(1536); // Vergroot naar 1536 om al de extra data met zekerheid kwijt te kunnen
  mqttClient.setCallback(mqttCallback);
  
  logToSyslogAndSerial("[MQTT] MQTT client geconfigureerd.");
}

/**
 * @brief Beheert de MQTT-verbinding en periodieke verzending in de loop.
 */
void handleMqtt() {
  if (WiFi.status() == WL_CONNECTED) {
    static bool mqttInitialized = false;
    if (!mqttInitialized) {
      mqttClient.setServer(mqtt_server, mqtt_port);
      mqttClient.setBufferSize(1536);
      mqttClient.setCallback(mqttCallback);
      mqttInitialized = true;
      logToSyslogAndSerial("[MQTT] Client op de achtergrond geconfigureerd.");
    }

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
 * @brief Verzamelt alle meetwaarden en publiceert deze via MQTT voor Grafana / Home Assistant.
 */
void sendMqttData() {
  if (millis() < 5000) {
    return; 
  }

  JsonDocument doc;
  
  // --- KAS DATA ---
  if (kasSmoothedTemp != -999.0) doc["kas_temp"] = kasSmoothedTemp;
  if (kasSmoothedHum != -999.0) doc["kas_vochtigheid"] = kasSmoothedHum;
  if (kasDewPoint != -999.0) doc["kas_dauwpunt"] = kasDewPoint;
  if (kasVpd != -999.0) doc["kas_vpd"] = kasVpd;
  doc["kas_dp_marge"]       = kasDpMargin; // Cruciaal voor condensrisico in Grafana

  // --- WONING SENSOR DATA ---
  if (indoorSmoothedTemp != -999.0) doc["binnen_temp"] = indoorSmoothedTemp;
  if (indoorSmoothedHum != -999.0) doc["binnen_vochtigheid"] = indoorSmoothedHum;
  if (indoorDewPoint != -999.0) doc["binnen_dauwpunt"] = indoorDewPoint;
  if (indoorVpd != -999.0) doc["binnen_vpd"] = indoorVpd;
  doc["binnen_dp_marge"]    = indoorDpMargin; // ⭐ Toegevoegd voor symmetrie!

  // --- BUITEN DATA ---
  if (outdoorTemp != -999.0) doc["buiten_temp"] = outdoorTemp;
  if (outdoorHumidity != -999.0) doc["buiten_vochtigheid"] = outdoorHumidity;
  if (outdoorDewPoint != -999.0) doc["buiten_dauwpunt"] = outdoorDewPoint;
  if (outdoorVpd != -999.0) doc["buiten_vpd"] = outdoorVpd;
  doc["buiten_dp_marge"]    = outdoorDpMargin; // ⭐ Toegevoegd voor symmetrie!
  
// --- BOOLEANS & RISICO STATUSSEN (Netjes als 1 en 0 voor InfluxDB/Grafana) ---
  doc["schimmel_risico"] = moldRisk;
  doc["uitdroging_risico"]   = (kasVpd > VPD_MAX_OPTIMAL) ? 1 : 0;
  doc["te_klam_risico"]      = (kasVpd < VPD_MIN_OPTIMAL || kasSmoothedHum >= HUM_MOLD_THRESHOLD) ? 1 : 0;
  doc["verwarmingsmat"]      = isHeatMatRecommended ? 1 : 0;
  doc["testmodus_actief"]    = isTestModeActive ? 1 : 0;

  // ⭐ Actuator advies vlaggen per component (0 = Nee / 1 = Ja)
  doc["advies_kas_actief"]   = (kasAdvice != OFF) ? 1 : 0;
  doc["advies_binnen_actief"] = (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) ? 1 : 0;
  doc["advies_buiten_actief"] = (kasAdvice == GREENHOUSE_VENTILATE) ? 1 : 0;
  
  // --- OVERIGE KLIMAAT & SYSTEEM DATA ---
  if (currentPressure > 0.0) doc["luchtdruk"] = currentPressure;
  doc["luchtdruk_trend"]    = baroTrendArrow;
  if (currentLuxValue >= 0.0) doc["licht_lux"] = currentLuxValue;

  // Systeemadviezen en redenen voor je dashboard logboek
  doc["schimmel_reden"]     = moldReasonText;
  doc["kas_advies_reden"]   = kasAdviceReason;
  doc["kas_vpd_status"]     = vpdStatusText;
  doc["dp_marge_status"]    = dpMarginStatusText;

  // Converteer de KasVentState enum naar een leesbare tekst string voor Grafana
  switch (kasAdvice) {
    case OFF: doc["kas_advies"] = "OFF"; break;
    case GREENHOUSE_VENTILATE: doc["kas_advies"] = "VENTILATE"; break;
    case GREENHOUSE_CIRCULATE_INTERNAL: doc["kas_advies"] = "CIRCULATE"; break;
    default: doc["kas_advies"] = "UNKNOWN"; break;
  }
  
  // --- VENTILATOR DATA ---
  doc["fan_int_rpm"]        = fanIntRPM;
  doc["fan_int_pct"]        = map(fanIntSpeed, 0, 255, 0, 100);
  
  doc["fan_ext1_rpm"]       = fanExt1RPM;
  doc["fan_ext1_pct"]       = map(fanExt1Speed, 0, 255, 0, 100);
  
  doc["fan_ext2_rpm"]       = fanExt2RPM;
  doc["fan_ext2_pct"]       = map(fanExt2Speed, 0, 255, 0, 100);

  // --- DIAGNOSTIEK ---
  doc["wifi_rssi"]          = wifiRSSI;
  doc["free_heap"]          = freeHeap;

  // Lees de interne temperatuur uit via de EspSensors module
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