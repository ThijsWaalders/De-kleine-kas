/**
 * @file OpenWeather.cpp
 * @brief Implementatie van actueel weer ophalen en 3-uurs voorspelling voor energiebesparende sturing op de ESP32-S3.
 */

#include "OpenWeather.h"
#include "Config.h"
#include "ClimateLogic.h"
#include "Logger.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

/**
 * @brief Verkort weersbeschrijvingen voor compacte weergave op het display.
 */
const char* shortenWeather(const String& text) {
  String lower = text;
  lower.toLowerCase();
  if (lower.indexOf("geheel bewolkt") >= 0 || lower.indexOf("overcast") >= 0) return "bewolkt";
  if (lower.indexOf("half bewolkt") >= 0 || lower.indexOf("clouds") >= 0) return "wolkjes";
  if (lower.indexOf("lichte regen") >= 0 || lower.indexOf("light rain") >= 0) return "l.regen";
  if (lower.indexOf("zware regen") >= 0 || lower.indexOf("heavy rain") >= 0) return "z.regen";
  if (lower.indexOf("onweersbui") >= 0 || lower.indexOf("thunderstorm") >= 0) return "onweer";
  if (lower.indexOf("clear") >= 0 || lower.indexOf("onbewolkt") >= 0) return "helder";
  return text.c_str();
}

/**
 * @brief Haalt actuele weergegevens op via de OpenWeatherMap API.
 */
void fetchInternetWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure weatherSslClient;
  weatherSslClient.setInsecure();
  weatherSslClient.setTimeout(2500); 

  HTTPClient http;
  http.setTimeout(2500); 
  String url = "https://api.openweathermap.org/data/2.5/weather?id=" + cityID + "&appid=" + openWeatherKey + "&units=metric&lang=nl";
  
  if (http.begin(weatherSslClient, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getStream())) {
        outdoorTemp = doc["main"]["temp"].as<float>();
        outdoorHumidity = doc["main"]["humidity"].as<float>();
        
        // Luchtdruk ophalen uit OpenWeather JSON (in hPa)
        if (doc["main"]["pressure"].is<float>()) {
          float newPressure = doc["main"]["pressure"].as<float>();
          if (outdoorPressure > 0.0) {
            if (newPressure > outdoorPressure + 0.5) outdoorPressureTrendText = "Stijgend 📈";
            else if (newPressure < outdoorPressure - 0.5) outdoorPressureTrendText = "Dalend 📉";
            else outdoorPressureTrendText = "Stabiel ⚖️";
          }
          outdoorPressure = newPressure;
        }

        // ZELF DAGELIJKSE MIN/MAX BUITEN BIJHOUDEN
        if (outdoorTempMin == 0.0 || outdoorTemp < outdoorTempMin) outdoorTempMin = outdoorTemp;
        if (outdoorTemp > outdoorTempMax) outdoorTempMax = outdoorTemp;

        const char* descStr = doc["weather"][0]["description"];
        weatherDesc = descStr ? String(descStr) : "Onbekend";
        weatherID = doc["weather"][0]["id"].as<int>();
        hasOutdoorAlert = (weatherID < 700); 
        outdoorDewPoint = calcDewPoint(outdoorTemp, outdoorHumidity);
        outdoorMoldRisk = (outdoorHumidity >= HUM_MOLD_THRESHOLD);
      } else {
        logToSyslogAndSerial("[WEATHER ERROR] Kon OpenWeather JSON niet parsen.");
      }
    } else {
      logToSyslogAndSerialPrintf("[WEATHER ERROR] HTTP-foutcode: %d", httpCode);
    }
    http.end();
  } else {
    logToSyslogAndSerial("[WEATHER ERROR] Kon geen verbinding maken met OpenWeather API.");
  }

  weatherSslClient.stop(); 
}

/**
 * @brief Haalt specifiek de weersvoorspelling voor de komende 3 uur op (cnt=2) om slim en zuinig te regelen.
 */
void fetchWeatherForecast() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure forecastSslClient;
  forecastSslClient.setInsecure();
  forecastSslClient.setTimeout(2500);

  HTTPClient http;
  http.setTimeout(2500);
  
  String url = "https://api.openweathermap.org/data/2.5/forecast?id=" + cityID + "&appid=" + openWeatherKey + "&units=metric&cnt=2";

  if (http.begin(forecastSslClient, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      JsonDocument doc;
      if (!deserializeJson(doc, http.getStream())) {
        
        JsonObject nextForecast = doc["list"][1];
        if (!nextForecast.isNull() && nextForecast["main"]["temp"].is<float>()) {
          forecastedTempSoon = nextForecast["main"]["temp"].as<float>();
        } else if (!doc["list"][0].isNull()) {
          forecastedTempSoon = doc["list"][0]["main"]["temp"].as<float>();
        }

        if (forecastedTempSoon != 0.0) {
          isTempDroppingSoon = (forecastedTempSoon < (outdoorTemp - 2.0));

          if (isTempDroppingSoon) {
            logToSyslogAndSerialPrintf("[SMART ENERGY] Temperatuur daalt over 3 uur naar %.1f°C. Anticiperen...", forecastedTempSoon);
          }
        }
      } else {
        logToSyslogAndSerial("[FORECAST ERROR] Kon Forecast JSON niet parsen.");
      }
    } else {
      logToSyslogAndSerialPrintf("[FORECAST ERROR] HTTP-foutcode: %d", httpCode);
    }
    http.end();
  }
  forecastSslClient.stop();
}