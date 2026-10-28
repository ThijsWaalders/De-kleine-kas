/**
 * @file MQTT.cpp
 * @brief Implementatie van MQTT-communicatie, offline LittleFS buffering en JSON-telemetrie voor ESP32-S3.
 */

#include "Config.h"
#include "MQTT.h"
#include "ClimateLogic.h"
#include "KasSensors.h" // Nodig voor traySensor1Temp en traySensor2Temp
#include "Logger.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// Maximale hoeveelheid opgeslagen regels tijdens netwerkuitval om flash-overbelasting te voorkomen
const int MAX_BUFFER_LINES = 50; 


// =========================================================================
// 📁 DEEL 1: OFFLINE BUFFER BEHEER (LITTLEFS)
// =========================================================================

/**
 * @brief Bewaakt en bewaart MQTT-payloads lokaal in LittleFS wanneer de verbinding wegvalt.
 */
void savePayloadToBuffer(const String& payload) {
  // Tel eerst hoeveel regels er al in het bufferbestand staan
  File checkFile = LittleFS.open(BUFFER_FILE, "r");
  int lineCount = 0;
  if (checkFile) {
    while (checkFile.available()) {
      if (checkFile.read() == '\n') lineCount++;
    }
    checkFile.close();
  }

  // Als de buffer vol is, weiger nieuwe toevoegingen om flash-wear tegen te gaan
  if (lineCount >= MAX_BUFFER_LINES) {
    logToSyslogAndSerial("[MQTT BUFFER WARNING] Buffer vol (" + String(MAX_BUFFER_LINES) + " regels). Oude metingen worden niet overschreven.");
    return;
  }

  // Voeg de payload toe aan het einde van het bufferbestand
  File file = LittleFS.open(BUFFER_FILE, "a");
  if (file) {
    file.println(payload);
    file.close();
    logToSyslogAndSerial("[MQTT BUFFER] Offline meting opgeslagen in LittleFS.");
  }
}

/**
 * @brief Non-blocking buffer flush: verwerkt exact 1 gebufferd bericht per interval zodra online.
 */
void processBufferedMqtt() {
  if (!mqttClient.connected() || !LittleFS.exists(BUFFER_FILE)) return;

  static unsigned long lastBufferFlushTime = 0;
  // Pauzeer minimaal 50ms tussen het verzenden van opeenvolgende bufferberichten
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

  // Lees de REST van het bestand in om later terug te schrijven
  String remainingBuffer = "";
  while (file.available()) {
    remainingBuffer += file.readStringUntil('\n') + "\n";
  }
  file.close();

  // Probeer het oudste gebufferde bericht te publiceren
  if (lineToSend.length() > 0) {
    if (mqttClient.publish(mqtt_topic, lineToSend.c_str())) {
      lastBufferFlushTime = millis();
      logToSyslogAndSerial("[MQTT BUFFER] Gebufferd bericht succesvol ingehaald!");

      // Werk het bestand bij met de resterende wachtende berichten
      if (remainingBuffer.length() > 0) {
        File outFile = LittleFS.open(BUFFER_FILE, "w");
        if (outFile) {
          outFile.print(remainingBuffer);
          outFile.close();
        }
      } else {
        LittleFS.remove(BUFFER_FILE); // Buffer is volledig leeg!
        logToSyslogAndSerial("[MQTT BUFFER] Alle gebufferde berichten succesvol ingehaald! Buffer gewist.");
      }
    }
  }
}


// =========================================================================
// 🌐 DEEL 2: NETWERK & MQTT VERBINDINGSBEHEER
// =========================================================================

/**
 * @brief Callback voor inkomende MQTT-berichten (indien van toepassing).
 */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Implementatie voor inkomende sturingen (indien gewenst)
}

/**
 * @brief Initialiseert de MQTT client configuratie en buffers.
 */
void setupMqtt() {
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setBufferSize(1536);
  mqttClient.setCallback(mqttCallback);
  logToSyslogAndSerial("[MQTT] MQTT client geconfigureerd.");
}

/**
 * @brief Handelt automatische herverbinding en Last Will & Testament (LWT) af.
 */
void reconnectMqtt() {
  if (!mqttClient.connected()) {
    uint64_t chipId = ESP.getEfuseMac();
    String clientId = "Esp32S3Kas-";
    clientId += String((uint32_t)(chipId >> 32), HEX);
    
    // Verbind met LWT topic (offline status bij plotselinge disconnect)
    if (mqttClient.connect(clientId.c_str(), mqtt_status_topic, 1, true, "offline")) {
      mqttClient.publish(mqtt_status_topic, "online", true); 
      logToSyslogAndSerial("[MQTT] Opnieuw verbonden met MQTT broker.");
    }
  }
}

/**
 * @brief Hoofdhandler voor netwerkstatus, verbinding en periodieke verzending in de loop.
 */
void handleMqtt() {
  if (WiFi.status() == WL_CONNECTED) {
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

    // Verwerk eventuele offline opgeslagen bufferberichten
    processBufferedMqtt();

    // Stuur nieuwe telemetrie op basis van het ingestelde interval
    static unsigned long lastMqttSend = 0;
    if (millis() - lastMqttSend >= MQTT_SEND_INTERVAL_MS) {
      lastMqttSend = millis();
      sendMqttData();
    }
  }
}

// =========================================================================
// 📊 DEEL 3: JSON PAYLOAD OPBOUW & PUBLICATIE
// =========================================================================

/**
 * @brief Verzamelt alle sensordata, statussen en adviezen, serialiseert naar JSON en publiceert of buffert.
 */
void sendMqttData() {
  if (millis() < 5000) return;

  JsonDocument doc;
  
  // --- 3.1 KAS DATA (SHT31) ---
  if (kasSmoothedTemp != -999.0) doc["kas_temp"] = kasSmoothedTemp;
  if (kasSmoothedHum != -999.0) doc["kas_vochtigheid"] = kasSmoothedHum;
  if (kasDewPoint != -999.0) doc["kas_dauwpunt"] = kasDewPoint;
  if (kasVpd != -999.0) doc["kas_vpd"] = kasVpd;
  doc["kas_dp_marge"] = kasDpMargin;

  // --- 3.2 TRAY SENSOREN (DS18B20) & MODUS GRENZEN ---
  if (traySensor1Temp != -999.0) doc["tray_sensor_1_temp"] = traySensor1Temp;
  doc["tray_1_mode"] = tray1Mode;
  doc["tray_1_min"]  = getTrayMinTemp(tray1Mode);
  doc["tray_1_max"]  = getTrayMaxTemp(tray1Mode);
  doc["tray_1_status"] = (tray1Mode != 3 && traySensor1Temp != -999.0 && (traySensor1Temp < getTrayMinTemp(tray1Mode) || traySensor1Temp > getTrayMaxTemp(tray1Mode))) ? 1 : 0;

  if (traySensor2Temp != -999.0) doc["tray_sensor_2_temp"] = traySensor2Temp;
  doc["tray_2_mode"] = tray2Mode;
  doc["tray_2_min"]  = getTrayMinTemp(tray2Mode);
  doc["tray_2_max"]  = getTrayMaxTemp(tray2Mode);
  doc["tray_2_status"] = (tray2Mode != 3 && traySensor2Temp != -999.0 && (traySensor2Temp < getTrayMinTemp(tray2Mode) || traySensor2Temp > getTrayMaxTemp(tray2Mode))) ? 1 : 0;

  // --- 3.3 WONING SENSOR DATA (BME280) ---
  if (indoorSmoothedTemp != -999.0) doc["binnen_temp"] = indoorSmoothedTemp;
  if (indoorSmoothedHum != -999.0) doc["binnen_vochtigheid"] = indoorSmoothedHum;
  if (indoorDewPoint != -999.0) doc["binnen_dauwpunt"] = indoorDewPoint;
  if (indoorVpd != -999.0) doc["binnen_vpd"] = indoorVpd;
  doc["binnen_dp_marge"] = indoorDpMargin;

  // --- 3.4 BUITEN DATA ---
  if (outdoorTemp != -999.0) doc["buiten_temp"] = outdoorTemp;
  if (outdoorHumidity != -999.0) doc["buiten_vochtigheid"] = outdoorHumidity;
  if (outdoorDewPoint != -999.0) doc["buiten_dauwpunt"] = outdoorDewPoint;
  
  // Strikte grenscheck voor buiten VPD (0.0 - 5.0 kPa)
  if (outdoorVpd != -999.0 && outdoorVpd >= 0.0 && outdoorVpd <= 5.0) {
    doc["buiten_vpd"] = outdoorVpd;
  }
  doc["buiten_dp_marge"] = outdoorDpMargin;
  
  // --- 3.5 RISICO STATUSSEN & ACTATORS ---
  doc["schimmel_risico"]     = moldRisk;
  
  float currentMaxVpdMqtt = lampsAreOn ? VPD_MAX_OPTIMAL_DAY : VPD_MAX_OPTIMAL_NIGHT;
  float currentMinVpdMqtt = lampsAreOn ? VPD_MIN_OPTIMAL_DAY : VPD_MIN_OPTIMAL_NIGHT;

  doc["uitdroging_risico"]   = (kasVpd > currentMaxVpdMqtt) ? 1 : 0;
  doc["te_klam_risico"]      = (kasVpd < currentMinVpdMqtt || kasSmoothedHum >= HUM_MOLD_THRESHOLD) ? 1 : 0;
  
  doc["verwarmingsmat"]      = isHeatMatRecommended ? 1 : 0;
  doc["testmodus_actief"]    = isTestModeActive ? 1 : 0;

  // --- 3.6 VENTILATIE & ZONE ADVIEZEN ---
  doc["advies_kas_actief"]    = (kasAdvice != KAS_OFF) ? 1 : 0;
  doc["advies_binnen_actief"] = (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) ? 1 : 0;
  doc["advies_buiten_actief"] = homeVentActive ? 1 :  0;  

  // Verrijkte kas-advies status voor Grafana (onderscheid tussen Rust en Blokkade)
  String kasAdviesText = "CLOSED_OPTIMAL";
  if (kasAdvice == GREENHOUSE_VENTILATE) {
    kasAdviesText = "VENTILATE";
  } else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
    kasAdviesText = "CIRCULATE";
  } else {
    // Als de kas dicht is: is het optimaal of worden we tegengehouden door buiten?
    if (moldRisk || kasSmoothedHum > 78.0f) {
      kasAdviesText = "CLOSED_BLOCKED"; // Vochtcorrectie nodig, maar buitenlucht laat het niet toe
    } else {
      kasAdviesText = "CLOSED_OPTIMAL"; // Alles is rustig en in balans
    }
  }
  doc["kas_advies"] = kasAdviesText;

  // Redenen en status-teksten  voor Grafana/Telegram
  doc["schimmel_reden"]     = moldReasonText;
  doc["kas_advies_reden"]   = kasAdviceReason;
  doc["kas_vpd_status"]     = vpdStatusText;
  doc["dp_marge_status"]    = dpMarginStatusText;
  
  // --- 3.7 VENTILATOR STATISTIEKEN (RPM & Percentage) ---
  doc["fan_int_rpm"]  = fanIntRPM;
  doc["fan_int_pct"]  = map(fanIntSpeed, 0, 255, 0, 100);
  doc["fan_ext1_rpm"] = fanExt1RPM;
  doc["fan_ext1_pct"] = map(fanExt1Speed, 0, 255, 0, 100);
  doc["fan_ext2_rpm"] = fanExt2RPM;
  doc["fan_ext2_pct"] = map(fanExt2Speed, 0, 255, 0, 100);

  // --- 3.8 SYSTEEM & DIAGNOSTIEK ---
  if (currentPressure > 0.0) doc["luchtdruk"] = currentPressure;
  doc["luchtdruk_trend"] = baroTrendArrow;
  if (currentLuxValue >= 0.0) doc["licht_lux"] = currentLuxValue;

  doc["wifi_rssi"]            = wifiRSSI;
  doc["free_heap"]            = freeHeap;
  doc["failed_kas_reads"]     = failedKasReads;
  doc["failed_indoor_reads"]  = failedIndoorReads;

  float espInternalTemp = readEspInternalTemp();
  if (espInternalTemp != -999.0 && espInternalTemp != 0.0) {
    doc["esp_temp"] = espInternalTemp;
  }

  // --- 3.9 SERIALISATIE & UITVOER / BUFFERING ---
  char buffer[1536];
  serializeJson(doc, buffer);

  if (mqttClient.connected()) {
    if (mqttClient.publish(mqtt_topic, buffer)) {
      lastSuccessfulNetworkActivity = millis(); 
    } else {
      savePayloadToBuffer(String(buffer)); // Publiceren mislukt, buffer lokaal
    }
  } else {
    savePayloadToBuffer(String(buffer));     // Geen verbinding, buffer lokaal
  }
}

