/**
 * @file Display.cpp
 * @brief Implementatie van het display met PIR-bewegingssensor sturing en directe statusweergave voor ESP32-S3.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "Display.h"

// Externe variabele ophalen uit ClimateLogic.cpp
extern bool isClimateCrash;

// Externe functies die elders in je project gedefinieerd zijn
const char* shortenWeather(const String& text);
void sendTelegramAlert(String message);
String buildStatusReport();

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
 * @brief Beheert het display op basis van de PIR-bewegingssensor (HW-416-B op Pin 1).
 * Scherm is standaard uit en springt aan bij beweging.
 */
void handleDisplayPower() {
  int motionState = digitalRead(PIN_PIR);
  
  if (motionState == HIGH) {
    lastMotionTime = millis();
    if (!isDisplayActiveByMotion) {
      isDisplayActiveByMotion = true;
      display.displayOn();
    }
  } else {
    // Schakel display na 30 seconden inactiviteit automatisch uit om inbranden te voorkomen
    if (isDisplayActiveByMotion && (millis() - lastMotionTime > 30000)) {
      isDisplayActiveByMotion = false;
      display.clear();
      display.display();
      display.displayOff();
      return;
    }
  }
}

void renderDisplay() {
  // 1. Controleer eerst of het display aan mag op basis van beweging
  handleDisplayPower();

  // Als er geen beweging is en het display is uitgeschakeld, render dan helemaal niets (energiebesparing & inbrandbeveiliging)
  if (!isDisplayActiveByMotion) {
    return;
  }

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  // Vaste helderheid wanneer actief (geen geklooi met lux-sensoren meer)
  display.setBrightness(120);

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
  
  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

  String statusText = "🟢 Kas stabiel";
  bool mustBlink = false;

  // 0. ABSOLUTE NOODTOESTAND: KLIMAAT CRASH OF UITDROGING (VPD te hoog voor kiemgroenten!)
  if (isClimateCrash || kasVpd > VPD_MAX_OPTIMAL) {
    statusText = "🔥 ALARM: KAS DROOGD UIT!";
    mustBlink = true; 
  }
  // 1. Winterslaap check
  else if (isKasSleeping) {
    statusText = "💤 Kas in winterslaap";
  }
  // 2. Opstartfase
  else if (millis() < 180000) {
    statusText = "⏳ Kas kalibreert...";
  }
  // 3. Testmodus weergave
  else if (isTestModeActive) {
    statusText = "⚡ TESTMODUS ACTIEF";
  }
  // 4. Actieve ventilatie / circulatie
  else if (kasAdvice == GREENHOUSE_VENTILATE) {
    if (fanExt1Pct > 0) {
      statusText = "💨 Afvoeren " + String(fanExt1Pct) + "%";
    } else {
      statusText = "🔒 Kas gesloten";
    }
  } 
  else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || fanIntPct > 0) {
    statusText = "🌀 Circuleren " + String(fanIntPct) + "%";
  } 
  else if (isHeatMatRecommended) {
    statusText = "🔥 Verwarmingsmat aan";
  }
  else if (moldRisk) {
    statusText = "⚠️ Schimmelrisico";
  }

  if (mustBlink && !blinkState) {
    statusText = ""; 
  }

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 52, statusText);

  display.display();
}

// /**
//  * @file Display.cpp
//  * @brief Implementatie van het display met strenge scheiding tussen actieve techniek en actie-voor-gebruiker voor ESP32-S3.
//  */

// #include "Config.h"
// #include "ClimateLogic.h"
// #include "Display.h"

// // Externe variabele ophalen uit ClimateLogic.cpp
// extern bool isClimateCrash;

// // Externe functies die elders in je project gedefinieerd zijn
// const char* shortenWeather(const String& text);
// void sendTelegramAlert(String message);
// String buildStatusReport();

// void drawBootScreen(String message) {
//   display.clear();
//   display.setFont(ArialMT_Plain_10);
//   display.setTextAlignment(TEXT_ALIGN_CENTER);
//   display.drawString(64, 2, F("De kleine kas"));
//   display.drawLine(0, 14, 128, 14);
//   display.drawString(64, 26, F("Opstarten..."));
//   display.drawString(64, 45, message);
//   display.display();
// }

// /**
//  * @brief Beheert de helderheid op basis van de kasverlichting (1 lux = uit, >1 lux = aan).
//  */
// void handleDisplayBrightness(float currentLux) {
//   const float LIGHT_THRESHOLD = 1.0;          

//   if (currentLux <= LIGHT_THRESHOLD) {
//     display.setBrightness(1);   // Zo gedimd mogelijk wanneer lampen uit zijn
//   } else {
//     display.setBrightness(110); // Half dimmend wanneer lampen aan zijn
//   }
// }

// void renderDisplay() {
//   handleDisplayBrightness(currentLuxValue);

//   display.clear();
//   display.setTextAlignment(TEXT_ALIGN_LEFT);
//   display.setFont(ArialMT_Plain_10);

//   // --- 1. BOVENSTE ZONE: Buitenweer & Knipperende Waarschuwingen ---
//   display.setColor(BLACK);
//   display.fillRect(0, 0, 128, 12);
//   display.setColor(WHITE);

//   bool blinkState = (millis() / 400) % 2 == 0; // Knippert actief (0.4s)

//   if (!isConnected) {
//     if (blinkState) display.drawString(0, 0, F("⚠ GEEN WIFI"));
//   } 
//   else if (hasOutdoorAlert && blinkState) {
//     display.drawString(0, 0, "ALARM: " + weatherDesc);
//   } 
//   else {
//     String outTempStr = (outdoorTemp <= -900) ? "N/A" : String(outdoorTemp, 1) + "C";
//     String outHumStr  = (outdoorHumidity <= -900) ? "" : " " + String(outdoorHumidity, 0) + "%";
//     display.drawString(0, 0, "BUITEN: " + outTempStr + outHumStr);
//   }

//   display.drawLine(0, 13, 128, 13);

//   // --- 2. MIDDEN ZONE: Grote Kas vs Woning Vergelijking ---
//   display.drawLine(64, 14, 64, 50);

//   // --- LINKERKOLOM: KAS ---
//   display.setFont(ArialMT_Plain_10);
//   display.drawString(0, 14, F("🪴 KAS"));

//   String kasTempStr = (displayedTemp <= -900) ? "N/A" : String(displayedTemp, 1) + "C";
//   display.drawString(0, 26, kasTempStr);

//   String kasHumStr = (kasSmoothedHum <= -900) ? "N/A" : String(kasSmoothedHum, 0) + "%";
//   display.drawString(0, 38, "LV:" + kasHumStr);

//   // --- RECHTERKOLOM: WONING ---
//   display.setFont(ArialMT_Plain_10);
//   display.drawString(68, 14, F("🏡 WONING"));

//   String houseTempStr = (indoorSmoothedTemp <= -900) ? "N/A" : String(indoorSmoothedTemp, 1) + "C";
//   display.drawString(68, 26, houseTempStr);

//   String houseHumStr = (indoorHum <= -900) ? "N/A" : String(indoorHum, 0) + "%";
//   display.drawString(68, 38, "LV:" + houseHumStr);

//   // --- 3. ONDERSTE ZONE: ESP-Automatisering vs. Menselijke Actie ---
//   display.drawLine(0, 51, 128, 51);
  
//   int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
//   int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

//   String statusText = "🟢 Kas draait zelfstandig";
//   bool mustBlink = false; // Alleen true bij actie of noodsituatie

//   // 0. ABSOLUTE NOODTOESTAND: KLIMAAT CRASH
//   if (isClimateCrash) {
//     statusText = "🚨 3 v/d 3 NOODKLIMATEN!!";
//     mustBlink = true; // Laat de noodmelding knipperen!
//   }
//   // 1. Winterslaap check
//   else if (isKasSleeping) {
//     statusText = "💤 ssst de kleine kas slaapt...";
//   }
//   // 2. Opstartfase
//   else if (millis() < 180000) {
//     statusText = "⏳ Kas kalibreert...";
//   }
//   // 3. KNIPPEREND: JIJ moet handelen (woning raam open)
//   else if (houseAdvice == HOUSE_VENTILATE && moldRisk) {
//     statusText = "🚨 VENTILEER DE WONING!";
//     mustBlink = true;
//   }
//   // 4. Testmodus weergave
//   else if (isTestModeActive) {
//     statusText = "⚡ TESTMODUS ACTIEF";
//   }
//   // 5. Wat doet de ESP automatisch in/voor de kas?
//   else if (kasAdvice == GREENHOUSE_VENTILATE) {
//     if (fanExt1Pct > 0) {
//       statusText = "💨 Kas voert af " + String(fanExt1Pct) + "%";
//     } else {
//       statusText = "🔒 De kleine kas is gesloten";
//     }
//   } 
//   else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || fanIntPct > 0) {
//     statusText = "🌀 Kas circuleert " + String(fanIntPct) + "%";
//   } 
//   else if (isHeatMatRecommended) {
//     statusText = "🔥 Verwarmingsmat Noodzakelijk";
//   }
//   // 6. Automatische risicobeheersing door ESP
//   else if (moldRisk) {
//     statusText = "⚠️ Schimmelrisico (kas regelt)";
//   }

//   // Als er actie/nood is, dwingen we het knipperen af via mustBlink
//   if (mustBlink && !blinkState) {
//     statusText = ""; 
//   }

//   display.setFont(ArialMT_Plain_10);
//   display.drawString(0, 52, statusText);

//   display.display();
// }