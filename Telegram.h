/**
 * @file Telegram.h
 * @brief Headerbestand voor Telegram bot communicatie, commando-afhandeling en rapportages (ESP32-S3).
 */

#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <Arduino.h>

// Functie prototypes voor Telegram functionaliteit
void sendBootNotification();
void sendTelegramAlert(String message);
String getResetReasonString(esp_reset_reason_t reason);
String buildStatusReport();
void sendHelpMenu();
void handleTelegramIncoming();

#endif // TELEGRAM_H