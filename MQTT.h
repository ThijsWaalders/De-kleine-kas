#ifndef MQTT_H
#define MQTT_H

#include <PubSubClient.h>
#include "NetworkManager.h" // Nodig voor logPrintln en WiFiClient

WiFiClient espClient;
PubSubClient mqttClient(espClient);

inline void setupMQTT() {
  mqttClient.setServer(IPAddress(SECRET_MQTT_SERVER_IP), 1883);
}

inline void handleMQTT() {
  if (!mqttClient.connected()) {
    logPrintln(F("[MQTT] Verbinding verbroken of niet actief. Opnieuw verbinden..."));
    while (!mqttClient.connected()) {
      logPrintln(F("[MQTT] Verbinden met broker... "));
      if (mqttClient.connect("ESP32S3Client")) {
        logPrintln(F("Verbonden!"));
      } else {
        logPrintln(F("Mislukt. Opnieuw proberen over 5 seconden."));
        delay(5000);
      }
    }
  }
  mqttClient.loop();
}

#endif