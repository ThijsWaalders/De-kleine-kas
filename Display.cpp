/**
 * @file Display.cpp
 * @brief Definitieve v3.6 implementatie: Vaste buitenbalk, roterend midden (Kas vs Trays), vaste Binnen-kolom en heldere status.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "KasSensors.h"
#include "Display.h"

// --- GLOBALE VARIABELEN ---
extern bool isClimateCrash;
extern float currentLuxValue; 
extern bool isSystemReady();
extern bool apModeActive;     

// Externe functies & variabelen
const char* shortenWeather(const String& text);
void sendTelegramAlert(String message);
String buildStatusReport();

void applyBrightness() {
  if (currentLuxValue <= 1.0) {
    display.setBrightness(10);  
  } else {
    display.setBrightness(100); 
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

void handleDisplayPower() {
  int motionState = digitalRead(PIN_PIR);
  unsigned long currentMillis = millis();
  
  if (motionState == HIGH) {
    lastMotionTime = currentMillis; 
    
    if (!isDisplayActiveByMotion) {
      isDisplayActiveByMotion = true;
      display.displayOn();            
      applyBrightness();              
      logToSyslogAndSerial("[PIR] Beweging gedetecteerd! Display ingeschakeld.");
    }
  }
  
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
  // AP / OTA MODUS
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
    display.drawString(64, 18, F("SSID: " AP_SSID));
    display.drawString(64, 32, F("IP: 192.168.2.1"));
    display.drawLine(0, 48, 128, 48);
    display.drawString(64, 50, F("Wachten op Flash..."));
    
    display.display();
    return; 
  }

  // =========================================================================
  // 1. BOVENSTE ZONE: Vaste Buitenbalk (Altijd rustig het buitenweer zichtbaar)
  // =========================================================================
  display.setColor(BLACK);
  display.fillRect(0, 0, 128, 12);
  display.setColor(WHITE);

  if (!isConnected) {
    if (blinkState) display.drawString(0, 0, F("⚠️ GEEN WIFI"));
  } 
  else if (hasOutdoorAlert && blinkState) {
    display.drawString(0, 0, "ALARM: " + weatherDesc);
  } 
  else {
    String outTempStr = (outdoorTemp <= -900) ? "N/A" : String(outdoorTemp, 1) + "C";
    String outHumStr  = (outdoorHumidity <= -900) ? "" : " " + String(outdoorHumidity, 0) + "%";
    display.drawString(0, 0, "BUITEN: " + outTempStr + " | LV:" + outHumStr);
  }

  display.drawLine(0, 13, 128, 13);

  // =========================================================================
  // 2. MIDDEN ZONE: Wisselt links tussen KAS en TRAYS | Rechts VAST BINNEN
  // =========================================================================
  display.drawLine(64, 14, 64, 50);

  // Fase schakelt elke 3000ms (0 = Kas klimaat, 1 = Tray 1 & 2 temperaturen)
  int midScreenPhase = (millis() / 3000) % 2;

  if (midScreenPhase == 0) {
    // LINKERKOLOM (Fase 0): KAS KLIMAAT
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 14, F("🪴 KAS"));
    String kasTempStr = (displayedTemp <= -900) ? "N/A" : String(displayedTemp, 1) + "C";
    display.drawString(0, 26, kasTempStr);
    String kasHumStr = (kasSmoothedHum <= -900) ? "N/A" : String(kasSmoothedHum, 0) + "%";
    display.drawString(0, 38, "LV:" + kasHumStr);
  } else {
    // LINKERKOLOM (Fase 1): TRAY SENSOREN
    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 14, F("🌱 TRAYS"));
    String t1 = traySensor1Connected ? String(traySensor1Temp, 1) + "C" : "Off";
    String t2 = traySensor2Connected ? String(traySensor2Temp, 1) + "C" : "Off";
    display.drawString(0, 26, "T1: " + t1);
    display.drawString(0, 38, "T2: " + t2);
  }

  // RECHTERKOLOM: BINNEN / WONING (Blijft altijd vast op dezelfde plek staan)
  display.setFont(ArialMT_Plain_10);
  display.drawString(68, 14, F("🏡 BINNEN"));
  String indoorTempStr = (indoorSmoothedTemp <= -900) ? "N/A" : String(indoorSmoothedTemp, 1) + "C";
  display.drawString(68, 26, indoorTempStr);
  String indoorHumStr = (indoorSmoothedHum <= -900) ? "N/A" : String(indoorSmoothedHum, 0) + "%";
  display.drawString(68, 38, "LV:" + indoorHumStr);

  // =========================================================================
  // 3. ONDERSTE ZONE: Roterend Informatielint (Status, Anticipatie & Actie)
  // =========================================================================
  display.drawLine(0, 51, 128, 51);
  display.setColor(BLACK);
  display.fillRect(0, 52, 128, 12);
  display.setColor(WHITE);

  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

  int displayPhase = (millis() / 2500) % 3;
  String statusLine = "";

  if (!isSystemReady()) {
    statusLine = "⏳ Kalibreren...";
  }
  else if (isKasSleeping) {
    statusLine = "😴 Winterslaap";
  }
  else if (isTestModeActive) {
    statusLine = "⚡ TESTMODUS";
  }
  else if (isClimateCrash || kasVpd > VPD_CRITICAL_HIGH) {
    statusLine = blinkState ? "🔥 ALARM: Hoge Temp!" : "🚨 VOCHT/KOEL ACTIE";
  }
  else if (isPreemptiveLampConditioningActive) {
    statusLine = "🔮 Lamp Anticipatie";
  }
  else {
    float maxOpt = lampsAreOn ? VPD_MAX_OPTIMAL_DAY : VPD_MAX_OPTIMAL_NIGHT;
    float minOpt = lampsAreOn ? VPD_MIN_OPTIMAL_DAY : VPD_MIN_OPTIMAL_NIGHT;
    bool hasRisk = (kasVpd > maxOpt || moldRisk || kasVpd < minOpt);
    
    String riskText = "";
    if (kasVpd > maxOpt)               riskText = "🔥 Lichte droogte";
    else if (moldRisk)                 riskText = "🌧️ Schimmelrisico";
    else if (kasVpd < minOpt)          riskText = "💧 Vochtophoping";

    String actionText = "🔒 Kas dicht (OK)";
    if (kasAdvice == GREENHOUSE_VENTILATE && fanExt1Pct > 0) {
      actionText = "💨 Ext Vent: " + String(fanExt1Pct) + "%";
    } else if (fanIntPct > 0) {
      actionText = "🌀 Interne Lucht";
    } else if (isHeatMatRecommended) {
      actionText = "🔥 Warmtemat AAN";
    }

    if (!hasRisk) {
      if (displayPhase % 2 == 0) {
        statusLine = "🟢 Klimaat Optimaal";
      } else {
        statusLine = actionText;
      }
    } else {
      if (displayPhase == 0) {
        statusLine = "🟡 Regelt bij...";
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
