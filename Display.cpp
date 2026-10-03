/**
 * @file Display.cpp
 * @brief Definitieve implementatie van het OLED-scherm met PIR-bewegingssensor en lux-dimming voor ESP32-S3-Zero.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "Display.h"

// --- GLOBALE VARIABELEN ---
extern bool isClimateCrash;
extern float currentLuxValue; // Haal de lux-waarde op uit de globale scope (bijv. uit je sensorbestand)

// Externe functies die elders in je project gedefinieerd zijn
const char* shortenWeather(const String& text);
void sendTelegramAlert(String message);
String buildStatusReport();

/**
 * @brief Stelt de helderheid in op basis van de omgevingslux.
 * Als lux <= 1.0 (donker/uit), wordt het scherm flink gedimd. Anders helder.
 */
void applyBrightness() {
  if (currentLuxValue <= 1.0) {
    display.setBrightness(10);  // Zacht gedimd in het donker (geen bouwlamp-effect meer)
  } else {
    display.setBrightness(100); // Normale helderheid overdag of bij brandende verlichting
  }
}

void drawBootScreen(String message) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 2, F("De kleine kas"));
  display.drawLine(0, 14, 128, 14);
  display.drawString(64, 26, F("Opstarten..."));
  display.drawString(64, 45, message);
  display.display();
}

/**
 * @brief Beheert het display op basis van de PIR-bewegingssensor (PIN_PIR).
 */
void handleDisplayPower() {
  int motionState = digitalRead(PIN_PIR);
  unsigned long currentMillis = millis();
  
  // 1. Als de sensor beweging ziet, ververs de timer direct
  if (motionState == HIGH) {
    lastMotionTime = currentMillis; // Reset de timer naar NU
    
    if (!isDisplayActiveByMotion) {
      isDisplayActiveByMotion = true;
      display.displayOn();            // Scherm hardware-matig aanzetten
      applyBrightness();              // Pas direct de juiste helderheid toe op basis van lux!
      logToSyslogAndSerial("[PIR] Beweging gedetecteerd! Display ingeschakeld.");
    }
  }
  
  // 2. Controleer of de 30 seconden inactiviteit voorbij zijn
  if (isDisplayActiveByMotion && (currentMillis - lastMotionTime > 30000)) {
    isDisplayActiveByMotion = false;
    
    // Maak het scherm leeg en schakel hardware-matig uit
    display.clear();
    display.display();
    display.displayOff();
    
    logToSyslogAndSerial("[PIR] Geen beweging meer. Display uitgeschakeld.");
  }
}

void renderDisplay() {
  // Als de timer nog op 0 staat (net opgestart), zet hem dan meteen op het huidige moment
  if (lastMotionTime == 0) {
    lastMotionTime = millis();
  }

  // Als het display uit staat door inactiviteit, teken dan helemaal niets
  if (!isDisplayActiveByMotion) {
    return;
  }

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  
  // Pas hier de helderheid dynamisch toe in plaats van een hardcoded 120!
  applyBrightness();

  // --- 1. BOVENSTE ZONE: Buitenweer & Knipperende Waarschuwingen ---
  display.setColor(BLACK);
  display.fillRect(0, 0, 128, 12);
  display.setColor(WHITE);

  bool blinkState = (millis() / 400) % 2 == 0; // Knippert actief (0.4s)

  if (!isConnected) {
    if (blinkState) display.drawString(0, 0, F("⚠ GEEN WIFI"));
  } 
  else if (hasOutdoorAlert && blinkState) {
    display.drawString(0, 0, "ALARM: " + weatherDesc);
  } 
  else {
    String outTempStr = (outdoorTemp <= -900) ? "N/A" : String(outdoorTemp, 1) + "C";
    String outHumStr  = (outdoorHumidity <= -900) ? "" : " " + String(outdoorHumidity, 0) + "%";
    display.drawString(0, 0, "BUITEN: " + outTempStr + outHumStr);
  }

  display.drawLine(0, 13, 128, 13);

  // --- 2. MIDDEN ZONE: Grote Kas vs Woning Vergelijking ---
  display.drawLine(64, 14, 64, 50);

  // --- LINKERKOLOM: KAS ---
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 14, F("🪴 KAS"));

  String kasTempStr = (displayedTemp <= -900) ? "N/A" : String(displayedTemp, 1) + "C";
  display.drawString(0, 26, kasTempStr);

  String kasHumStr = (kasSmoothedHum <= -900) ? "N/A" : String(kasSmoothedHum, 0) + "%";
  display.drawString(0, 38, "LV:" + kasHumStr);

  // --- RECHTERKOLOM: WONING ---
  display.setFont(ArialMT_Plain_10);
  display.drawString(68, 14, F("🏡 WONING"));

  String houseTempStr = (indoorSmoothedTemp <= -900) ? "N/A" : String(indoorSmoothedTemp, 1) + "C";
  display.drawString(68, 26, houseTempStr);

  String houseHumStr = (indoorHum <= -900) ? "N/A" : String(indoorHum, 0) + "%";
  display.drawString(68, 38, "LV:" + houseHumStr);

  // --- 3. ONDERSTE ZONE: Directe Status & Alarmering ---
  display.drawLine(0, 51, 128, 51);
  
  display.setColor(BLACK);
  display.fillRect(0, 52, 128, 12);
  display.setColor(WHITE);

  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

  String normalStatus = "🟢 Kas stabiel";

  if (millis() < 180000) {
    normalStatus = "⏳ Kalibreren...";
  }
  else if (isKasSleeping) {
    normalStatus = "💤 Winterslaap";
  }
  else if (isTestModeActive) {
    normalStatus = "⚡ TESTMODUS";
  }
  else if (kasAdvice == GREENHOUSE_VENTILATE) {
    if (fanExt1Pct > 0) {
      normalStatus = "💨 Afvoer: " + String(fanExt1Pct) + "%";
    } else {
      normalStatus = "🔒 Kas gesloten";
    }
  } 
  else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || fanIntPct > 0) {
    normalStatus = "🌀 Circulatie: " + String(fanIntPct) + "%";
  } 
  else if (isHeatMatRecommended) {
    normalStatus = "🔥 Mat aan";
  }
  else if (moldRisk) {
    normalStatus = "⚠️ Schimmelrisico";
  }

  String finalStatusText = normalStatus;
  bool isEmergencyAlarm = (isClimateCrash || kasVpd > VPD_MAX_OPTIMAL);

  if (isEmergencyAlarm) {
    if (blinkState) {
      finalStatusText = "🔥 ALARM: DROOGTE!";
    } else {
      finalStatusText = normalStatus; 
    }
  }

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 52, finalStatusText);

  display.display();
}
