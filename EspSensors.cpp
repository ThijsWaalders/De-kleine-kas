/**
 * @file EspSensors.cpp
 * @brief Beheert de interne sensoren en hardware-veiligheid van de ESP32-S3 (moderne ESP-IDF API).
 */

#include "EspSensors.h"
#include "Config.h"

// Externe functie uit Telegram / main code
extern void sendTelegramAlert(String message);

unsigned long lastEspTempCheck = 0;
float espInternalTemp = 0.0;
static temperature_sensor_handle_t temp_sensor_handle = NULL;

// Herhalend alarm variabelen
static unsigned long lastEspAlertTime = 0;
const unsigned long ESP_ALERT_INTERVAL = 900000; // 15 minuten in milliseconden
// const float ESP_CRITICAL_TEMP = 75.0;            // Veiligheidsgrens chip-temperatuur

void initEspSensors() {
  // Installeer de interne temperatuursensor voor de ESP32-S3 (meetbereik ingesteld op 20°C tot 100°C)
  temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
  
  if (temperature_sensor_install(&temp_sensor_config, &temp_sensor_handle) == ESP_OK) {
    temperature_sensor_enable(temp_sensor_handle);
  }
}

float readEspInternalTemp() {
  if (temp_sensor_handle == NULL) return 0.0;

  float result = 0.0;
  if (temperature_sensor_get_celsius(temp_sensor_handle, &result) == ESP_OK) {
    espInternalTemp = result;
  }
  return espInternalTemp;
}

void checkEspTemperatureSafety() {
  if (isnan(espInternalTemp) || espInternalTemp == 0.0) return;

  // Als de chip te heet wordt door bijv. stroomlekken via GPIO's
  if (espInternalTemp >= ESP_CRITICAL_TEMP) {
    unsigned long currentMillis = millis();

    if (lastEspAlertTime == 0 || (currentMillis - lastEspAlertTime >= ESP_ALERT_INTERVAL)) {
      lastEspAlertTime = currentMillis;
      
      String msg = "🔥 *ESP32 HARDWARE OVERVERHITTINGS-ALARM!*\n"
                   "Interne chip temperatuur is kritiek hoog: *" + String(espInternalTemp, 1) + "°C*!\n\n"
                   "⚠️ *Mogelijke oorzaak:* Controleer direct de bedrading en de gezamenlijke massa (GND) van de fans (mogelijk lekkende stroom / back-feeding op GPIO 8-13).";
      
      sendTelegramAlert(msg);
    }
  } else {
    // Hersteld onder de grens
    if (lastEspAlertTime != 0) {
      sendTelegramAlert("✅ *ESP32 HERSTEL:* Chip temperatuur is weer veilig gezakt (" + String(espInternalTemp, 1) + "°C).");
      lastEspAlertTime = 0;
    }
  }
}