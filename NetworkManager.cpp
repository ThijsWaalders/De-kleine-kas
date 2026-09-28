#include "NetworkManager.h"

WiFiServer telnetServer(23);
WiFiClient telnetClient;

void logPrint(const String &msg) {
  Serial.print(msg);
  if (telnetClient && telnetClient.connected()) telnetClient.print(msg);
}

void logPrintln(const String &msg) {
  Serial.println(msg);
  if (telnetClient && telnetClient.connected()) telnetClient.println(msg);
}

void logPrint(const __FlashStringHelper *msg) {
  Serial.print(msg);
  if (telnetClient && telnetClient.connected()) telnetClient.print(msg);
}

void logPrintln(const __FlashStringHelper *msg) {
  Serial.println(msg);
  if (telnetClient && telnetClient.connected()) telnetClient.println(msg);
}

void setupNetwork() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); 
  
  if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET, PRIMARY_DNS)) {
    Serial.println(F("[WiFi] Configuratie van vast IP mislukt!"));
  }
  
  WiFi.begin(SECRET_SSID, SECRET_PASS, WIFI_CHANNEL);
  
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");

    if (millis() - startAttemptTime > 20000) {
      WiFi.disconnect();
      delay(1000);
      WiFi.begin(SECRET_SSID, SECRET_PASS, WIFI_CHANNEL);
      startAttemptTime = millis();
    }
  }
  
  telnetServer.begin();
  telnetServer.setNoDelay(true);

  logPrintln(F("\n\n=========================================="));
  logPrintln(String(F("[BOOT] ")) + String(HOSTNAME) + String(F(" wordt gestart...")));
  logPrintln(F("=========================================="));
  
  logPrintln(F("[WiFi] Verbonden!"));
  logPrintln(String(F("[WiFi] IP-adres: ")) + WiFi.localIP().toString());
  logPrintln(F("[Telnet] Telnet server gestart op poort 23"));

  ArduinoOTA.setHostname(HOSTNAME);
  
  ArduinoOTA.onStart([]() {
    logPrintln(F("\n[OTA] Update gestart..."));
  });
  
  ArduinoOTA.onEnd([]() { 
    logPrintln(F("\n[OTA] Update klaar! Systeem herstart...")); 
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    char buf[32];
    snprintf(buf, sizeof(buf), "[OTA] Voortgang: %u%%\r", (progress / (total / 100)));
    Serial.print(buf);
    if (telnetClient && telnetClient.connected()) telnetClient.print(buf);
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    if (error == OTA_AUTH_ERROR) logPrintln(F("[OTA] Fout: Auth Failed"));
    else if (error == OTA_BEGIN_ERROR) logPrintln(F("[OTA] Fout: Begin Failed"));
    else if (error == OTA_CONNECT_ERROR) logPrintln(F("[OTA] Fout: Connect Failed"));
    else if (error == OTA_RECEIVE_ERROR) logPrintln(F("[OTA] Fout: Receive Failed"));
    else if (error == OTA_END_ERROR) logPrintln(F("[OTA] Fout: End Failed"));
  });

  ArduinoOTA.begin();
  logPrintln(F("[OTA] Klaar voor draadloze updates!"));
  logPrintln(F("==========================================\n"));
}

void handleNetwork() {
  if (telnetServer.hasClient()) {
    if (!telnetClient || !telnetClient.connected()) {
      if (telnetClient) telnetClient.stop();
      telnetClient = telnetServer.available();
      logPrintln(F("[Telnet] Nieuwe verbinding geaccepteerd!"));
    } else {
      WiFiClient newClient = telnetServer.available();
      newClient.stop();
    }
  }

  ArduinoOTA.handle();
}