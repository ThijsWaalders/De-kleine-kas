/**
 * @file Mqtt.h
 * @brief Headerbestand voor MQTT-communicatie, buffering en Home Assistant datastromen (ESP32-S3).
 */

#ifndef MQTT_H
#define MQTT_H

#include <Arduino.h>
#include "Config.h"

// Hoofdfuncties voor de .ino
void setupMqtt();
void handleMqtt();

// Interne helpers
void savePayloadToBuffer(String payload);
void flushBufferToMqtt();
void reconnectMqtt();
void sendMqttData();

#endif // MQTT_H