/**
 * @file OpenWeather.h
 * @brief Headerbestand voor het ophalen van actueel weer en de voorspelling voor de komende 3 uur (ESP32-S3).
 */

#ifndef OPEN_WEATHER_H
#define OPEN_WEATHER_H

#include <Arduino.h>

// Externe variabelen voor de voorspelling van de komende 3 uur
extern float forecastedTempSoon;     // Verwachte temperatuur over ~3 uur
extern bool isTempDroppingSoon;      // Signaal of er een koude omslag aankomt

void fetchInternetWeather();
// // Functie prototypes
// const char* shortenWeather(const String& text);
// // void fetchInternetWeather();
// if (WiFi.status() == WL_CONNECTED) {
//     fetchInternetWeather();
// }

void fetchWeatherForecast();


#endif // OPEN_WEATHER_H