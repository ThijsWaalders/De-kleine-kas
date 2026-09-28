/**
 * @file NetworkManager.h
 * @brief Headerbestand voor netwerkbeheer, Wi-Fi herverbindingslogica (state machine), RTC crash-analyse en Core 0 OTA op ESP32-S3.
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>

// Vereenvoudigde statussen voor het automatische herverbindingsmechanisme
enum WifiReconnectState { 
  WIFI_IDLE, 
  WIFI_WAITING_DROP_GRACE, 
  WIFI_TRYING_SSID1
};

/**
 * @struct RtcData
 * @brief Structuur om RTC user memory te benutten voor crash-analyse en SSID tracking.
 */
struct RtcData {
  uint32_t crc32;
  char last_ssid[32];
};

// Functie prototypes
void beginWithStaticIP(const char* targetSsid, const char* targetPassword);
void otaBackgroundCode(void * pvParameters);
void setupOtaTask();
void handleRtcConnectionLogging();
void handleWiFiReconnect();
void setupNetwork();
void handleNetwork();

#endif // NETWORK_MANAGER_H