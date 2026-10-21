/**
 * @file Display.cpp
 * @brief Definitieve implementatie van het OLED-scherm met PIR-bewegingssensor, lux-dimming en heldere statusweergave voor ESP32-S3-Zero.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "Display.h"

// --- GLOBALE VARIABELEN ---
extern bool isClimateCrash;
extern float currentLuxValue; // Haal de lux-waarde op uit de globale scope
extern bool isSystemReady();
extern bool apModeActive;     // Vlag voor actieve Access Point / OTA-modus

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
    display.setBrightness(10);  // Zacht gedimd in het donker
  } else {
    display.setBrightness(100); // Normale helderheid overdag of bij verlichting
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
    lastMotionTime = currentMillis; 
    
    if (!isDisplayActiveByMotion) {
      isDisplayActiveByMotion = true;
      display.displayOn();            
      applyBrightness();              
      logToSyslogAndSerial("[PIR] Beweging gedetecteerd! Display ingeschakeld.");
    }
  }
  
  // 2. Controleer of de 30 seconden inactiviteit voorbij zijn (tenzij in AP/OTA-modus: dan blijft scherm aan)
  if (isDisplayActiveByMotion && !apModeActive && (currentMillis - lastMotionTime > 30000)) {
    isDisplayActiveByMotion = false;
    
    display.clear();
    display.display();
    display.displayOff();
    
    logToSyslogAndSerial("[PIR] Geen beweging meer. Display uitgeschakeld.");
  }
}

void renderDisplay() {
  if (lastMotionTime == 0) {
    lastMotionTime = millis();
  }

  // Zorg dat het scherm altijd actief is als AP-modus actief is
  if (apModeActive && !isDisplayActiveByMotion) {
    isDisplayActiveByMotion = true;
    display.displayOn();
  }

  if (!isDisplayActiveByMotion) {
    return;
  }

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  
  applyBrightness();

  bool blinkState = (millis() / 400) % 2 == 0;

  // =========================================================================
  // SPECIFIEKE WEERGAVE VOOR AP / OTA MODUS
  // =========================================================================
  if (apModeActive) {
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    
    if (blinkState) {
      display.drawString(64, 0, F("🚀 OTA AP MODUS"));
    } else {
      display.drawString(64, 0, F("   OTA AP MODUS   "));
    }
    
    display.drawLine(0, 13, 128, 13);
    
    display.setFont(ArialMT_Plain_10);
    display.drawString(64, 18, F("SSID: " AP_SSID));
    display.drawString(64, 32, F("IP: 192.168.2.1"));
    
    display.drawLine(0, 48, 128, 48);
    display.drawString(64, 50, F("Wachten op Flash..."));
    
    display.display();
    return; // Sla de reguliere klimaat-render over zolang we in AP-modus zitten
  }

  // --- 1. BOVENSTE ZONE: Buitenweer ---
  display.setColor(BLACK);
  display.fillRect(0, 0, 128, 12);
  display.setColor(WHITE);

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

  // --- 2. MIDDEN ZONE: Kas vs Woning ---
  display.drawLine(64, 14, 64, 50);

  // LINKERKOLOM: KAS
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 14, F("🪴 KAS"));
  String kasTempStr = (displayedTemp <= -900) ? "N/A" : String(displayedTemp, 1) + "C";
  display.drawString(0, 26, kasTempStr);
  String kasHumStr = (kasSmoothedHum <= -900) ? "N/A" : String(kasSmoothedHum, 0) + "%";
  display.drawString(0, 38, "LV:" + kasHumStr);

  // RECHTERKOLOM: WONING
  display.setFont(ArialMT_Plain_10);
  display.drawString(68, 14, F("🏡 WONING"));
  String houseTempStr = (indoorSmoothedTemp <= -900) ? "N/A" : String(indoorSmoothedTemp, 1) + "C";
  display.drawString(68, 26, houseTempStr);
  String houseHumStr = (indoorHum <= -900) ? "N/A" : String(indoorHum, 0) + "%";
  display.drawString(68, 38, "LV:" + houseHumStr);

  // --- 3. ONDERSTE ZONE: Roterend Informatielint (Elke 2.5 seconden) ---
  display.drawLine(0, 51, 128, 51);
  display.setColor(BLACK);
  display.fillRect(0, 52, 128, 12);
  display.setColor(WHITE);

  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

  // Bepaal schermfase: 0, 1 of 2 (wisselt elke 2500ms)
  int displayPhase = (millis() / 2500) % 3;
  String statusLine = "";

  if (!isSystemReady()) {
    statusLine = "⏳ Kalibreren...";
  }
  else if (isKasSleeping) {
    statusLine = "💤 Winterslaap";
  }
  else if (isTestModeActive) {
    statusLine = "⚡ TESTMODUS";
  }
  else if (isClimateCrash || kasVpd > VPD_CRITICAL_HIGH) {
    statusLine = blinkState ? "🔥 ALARM: UITDROGING!" : "🚨 INGRIJPEN VEREIST";
  }
  else {
    // --- Bepaal Risico en Actie ---
    bool hasRisk = (kasVpd > VPD_MAX_OPTIMAL || moldRisk || kasVpd < VPD_MIN_OPTIMAL);
    
    String riskText = "";
    if (kasVpd > VPD_MAX_OPTIMAL)     riskText = "🔥 Risico: Uitdroging";
    else if (moldRisk)                 riskText = "🌧️ Risico: Schimmel";
    else if (kasVpd < VPD_MIN_OPTIMAL) riskText = "💧 Risico: Te klam";

    String actionText = "🔒 Kas rust / dicht";
    if (kasAdvice == GREENHOUSE_VENTILATE && fanExt1Pct > 0) {
      actionText = "💨 Ext Vent: " + String(fanExt1Pct) + "%";
    } else if (fanIntPct > 0) {
      actionText = "🌀 Int Circ: " + String(fanIntPct) + "%";
    } else if (isHeatMatRecommended) {
      actionText = "🔥 Warmtemat AAN";
    }

    // --- Roterende Carrousel Logica ---
    if (!hasRisk) {
      // Klimaat is 100% OK -> Wissel alleen tussen Status en Actie
      if (displayPhase % 2 == 0) {
        statusLine = "🟢 Klimaat OK";
      } else {
        statusLine = actionText;
      }
    } else {
      // Er is een Risico -> Roteer netjes langs 1, 2 en 3
      if (displayPhase == 0) {
        statusLine = "🟡 Wordt bijgesteld";
      } else if (displayPhase == 1) {
        statusLine = riskText;
      } else {
        statusLine = actionText;
      }
    }
  }

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 52, statusLine);

  display.display();
}
