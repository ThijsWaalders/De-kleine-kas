#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <IPAddress.h>

// ==========================================
// PROJECT & HOSTATTRIBUIUTEN
// ==========================================
#define HOSTNAME "ESP32-S3-Kas" // Verander dit per project

// ==========================================
// NETWERK & WIFI INSTELLINGEN
// ==========================================
// Haal je gevoelige data uit nono.h of vul ze hier in:
#include "nono.h" 
// nono.h bevat idealiter:
// #define SECRET_SSID "jouw_wifi_ssid"
// #define SECRET_PASS "jouw_wifi_wachtwoord"

// Vast IP-adres configuratie voor dit apparaat
const IPAddress LOCAL_IP(192, 168, 1, 201);
const IPAddress GATEWAY(192, 168, 1, 1);
const IPAddress SUBNET(255, 255, 255, 0);
const IPAddress PRIMARY_DNS(192, 168, 1, 110);

// WiFi kanaal (optioneel, versnelt verbinding als je het weet)
#define WIFI_CHANNEL 6

// ==========================================
// HARDWARE PINOUT & CONFIGURATIE
// ==========================================
#define PIN_NEOPIXEL 21
#define NUMPIXELS    1
#define LED_BRIGHTNESS 5 // Zachtjes om je netvlies te redden (0-255)

#endif