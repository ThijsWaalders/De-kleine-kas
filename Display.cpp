/**
 * @file Display.cpp
 * @brief Definitieve implementatie van het OLED-scherm met PIR-bewegingssensor sturing voor ESP32-S3-Zero.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "Display.h"

// --- GLOBALE VARIABELEN VOOR DISPLAY & PIR ---
// bool isDisplayActiveByMotion = true;   // <--- Zet deze op TRUE zodat het scherm na boot direct aan blijft!
// unsigned long lastMotionTime = 0;      // Wordt hieronder automatisch geactiveerd

// Externe variabele ophalen uit ClimateLogic.cpp / Config.cpp
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
 * @brief Beheert het display op basis van de PIR-bewegingssensor (PIN_PIR).
 * Scherm is standaard uit en springt aan bij beweging.
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
      display.setBrightness(120);     // Helderheid instellen
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
  
  // Maak het onderste vlak (van y=52 tot y=63) eerst volledig zwart om ghosting te voorkomen
  display.setColor(BLACK);
  display.fillRect(0, 52, 128, 12);
  display.setColor(WHITE);

  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

  // STAP 1: Bepaal ALTIJD eerst wat de kas aan het doen is (de normale status)
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

  // STAP 2: Controleer of er een echt noodalarm is
  String finalStatusText = normalStatus;
  bool isEmergencyAlarm = (isClimateCrash || kasVpd > VPD_MAX_OPTIMAL);

  if (isEmergencyAlarm) {
    // Knipper TUSSEN het alarm en wat de kas aan het doen is!
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
  // // --- 3. ONDERSTE ZONE: Directe Status & Alarmering ---
  // display.drawLine(0, 51, 128, 51);
  
  // int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  // int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

  // String statusText = "🟢 Kas stabiel";
  // bool mustBlink = false;

  // // 0. ABSOLUTE NOODTOESTAND: KLIMAAT CRASH OF UITDROGING
  // if (isClimateCrash || kasVpd > VPD_MAX_OPTIMAL) {
  //   statusText = "🔥 ALARM: KAS DROOGD UIT!";
  //   mustBlink = true; 
  // }
  // // 1. Winterslaap check
  // else if (isKasSleeping) {
  //   statusText = "💤 Kas in winterslaap";
  // }
  // // 2. Opstartfase
  // else if (millis() < 180000) {
  //   statusText = "⏳ Kas kalibreert...";
  // }
  // // 3. Testmodus weergave
  // else if (isTestModeActive) {
  //   statusText = "⚡ TESTMODUS ACTIEF";
  // }
  // // 4. Actieve ventilatie / circulatie
  // else if (kasAdvice == GREENHOUSE_VENTILATE) {
  //   if (fanExt1Pct > 0) {
  //     statusText = "💨 Afvoeren " + String(fanExt1Pct) + "%";
  //   } else {
  //     statusText = "🔒 Kas gesloten";
  //   }
  // } 
  // else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || fanIntPct > 0) {
  //   statusText = "🌀 Circuleren " + String(fanIntPct) + "%";
  // } 
  // else if (isHeatMatRecommended) {
  //   statusText = "🔥 Verwarmingsmat aan";
  // }
  // else if (moldRisk) {
  //   statusText = "⚠️ Schimmelrisico";
  // }

  // if (mustBlink && !blinkState) {
  //   statusText = ""; 
  // }

  // display.setFont(ArialMT_Plain_10);
  // display.drawString(0, 52, statusText);

  // display.display();
// }

// /**
//  * @file Display.cpp
//  * @brief Implementatie van het display met PIR-bewegingssensor sturing en directe statusweergave voor ESP32-S3.
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
//  * @brief Beheert het display op basis van de PIR-bewegingssensor (HW-416-B op Pin 1).
//  * Scherm is standaard uit en springt aan bij beweging.
//  */
// void handleDisplayPower() {
//   int motionState = digitalRead(PIN_PIR);
  
//   // 1. Als de sensor beweging ziet, ververs de timer direct
//   if (motionState == HIGH) {
//     lastMotionTime = millis(); // Reset de timer naar NU
    
//     if (!isDisplayActiveByMotion) {
//       isDisplayActiveByMotion = true;
//       display.displayOn();
//       logToSyslogAndSerial("[PIR] Beweging gedetecteerd! Display ingeschakeld.");
//     }
//   }
  
//   // 2. ONAFHANKELIJK van de sensor-status: controleer ALTIJD of de 30 seconden voorbij zijn
//   if (isDisplayActiveByMotion && (millis() - lastMotionTime > 30000)) {
//     isDisplayActiveByMotion = false;
    
//     // Forceer het scherm volledig zwart
//     display.clear();
//     display.setColor(BLACK);
//     display.fillRect(0, 0, 128, 64);
//     display.display();
//     display.setColor(WHITE); // Zet kleur terug voor de volgende keer
    
//     display.displayOff();
//     logToSyslogAndSerial("[PIR] Geen beweging meer. Display uitgeschakeld.");
//   }
// }
// // void handleDisplayPower() {
// //   int motionState = digitalRead(PIN_PIR);
// //   Serial.print("DEBUG PIR Pin State: "); Serial.println(motionState); // <--- Voeg dit toe!
  
// //   if (motionState == HIGH) {
// //     lastMotionTime = millis();
// //     if (!isDisplayActiveByMotion) {
// //       isDisplayActiveByMotion = true;
// //       display.displayOn();
// //     }
// //   } else {
// //     // Schakel display na 30 seconden inactiviteit automatisch uit
// //     if (isDisplayActiveByMotion && (millis() - lastMotionTime > 30000)) {
// //       isDisplayActiveByMotion = false;
      
// //       // Forceer het scherm volledig zwart (alle pixels uit)
// //       display.clear();
// //       display.setColor(BLACK);
// //       display.fillRect(0, 0, 128, 64);
// //       display.display();
      
// //       // Probeer optioneel de hardware sleep (als je scherm dat ondersteunt)
// //       display.displayOff();
// //       return;
// //     }
// //   }
// // }

// // void renderDisplay() {
// //   // 1. Controleer eerst of het display aan mag op basis van beweging
// //   handleDisplayPower();

// //   // Als er geen beweging is en het display is uitgeschakeld, render dan helemaal niets (energiebesparing & inbrandbeveiliging)
// //   if (!isDisplayActiveByMotion) {
// //     return;
// //   }

// //   display.clear();
// //   display.setTextAlignment(TEXT_ALIGN_LEFT);
// //   display.setFont(ArialMT_Plain_10);

// //   // Vaste helderheid wanneer actief (geen geklooi met lux-sensoren meer)
// //   display.setBrightness(120);

// //   // --- 1. BOVENSTE ZONE: Buitenweer & Knipperende Waarschuwingen ---
// //   display.setColor(BLACK);
// //   display.fillRect(0, 0, 128, 12);
// //   display.setColor(WHITE);

// //   bool blinkState = (millis() / 400) % 2 == 0; // Knippert actief (0.4s)

// //   if (!isConnected) {
// //     if (blinkState) display.drawString(0, 0, F("⚠ GEEN WIFI"));
// //   } 
// //   else if (hasOutdoorAlert && blinkState) {
// //     display.drawString(0, 0, "ALARM: " + weatherDesc);
// //   } 
// //   else {
// //     String outTempStr = (outdoorTemp <= -900) ? "N/A" : String(outdoorTemp, 1) + "C";
// //     String outHumStr  = (outdoorHumidity <= -900) ? "" : " " + String(outdoorHumidity, 0) + "%";
// //     display.drawString(0, 0, "BUITEN: " + outTempStr + outHumStr);
// //   }

// //   display.drawLine(0, 13, 128, 13);

// //   // --- 2. MIDDEN ZONE: Grote Kas vs Woning Vergelijking ---
// //   display.drawLine(64, 14, 64, 50);

// //   // --- LINKERKOLOM: KAS ---
// //   display.setFont(ArialMT_Plain_10);
// //   display.drawString(0, 14, F("🪴 KAS"));

// //   String kasTempStr = (displayedTemp <= -900) ? "N/A" : String(displayedTemp, 1) + "C";
// //   display.drawString(0, 26, kasTempStr);

// //   String kasHumStr = (kasSmoothedHum <= -900) ? "N/A" : String(kasSmoothedHum, 0) + "%";
// //   display.drawString(0, 38, "LV:" + kasHumStr);

// //   // --- RECHTERKOLOM: WONING ---
// //   display.setFont(ArialMT_Plain_10);
// //   display.drawString(68, 14, F("🏡 WONING"));

// //   String houseTempStr = (indoorSmoothedTemp <= -900) ? "N/A" : String(indoorSmoothedTemp, 1) + "C";
// //   display.drawString(68, 26, houseTempStr);

// //   String houseHumStr = (indoorHum <= -900) ? "N/A" : String(indoorHum, 0) + "%";
// //   display.drawString(68, 38, "LV:" + houseHumStr);

// //   // --- 3. ONDERSTE ZONE: Directe Status & Alarmering ---
// //   display.drawLine(0, 51, 128, 51);
  
// //   int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
// //   int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

// //   String statusText = "🟢 Kas stabiel";
// //   bool mustBlink = false;

// //   // 0. ABSOLUTE NOODTOESTAND: KLIMAAT CRASH OF UITDROGING (VPD te hoog voor kiemgroenten!)
// //   if (isClimateCrash || kasVpd > VPD_MAX_OPTIMAL) {
// //     statusText = "🔥 ALARM: KAS DROOGD UIT!";
// //     mustBlink = true; 
// //   }
// //   // 1. Winterslaap check
// //   else if (isKasSleeping) {
// //     statusText = "💤 Kas in winterslaap";
// //   }
// //   // 2. Opstartfase
// //   else if (millis() < 180000) {
// //     statusText = "⏳ Kas kalibreert...";
// //   }
// //   // 3. Testmodus weergave
// //   else if (isTestModeActive) {
// //     statusText = "⚡ TESTMODUS ACTIEF";
// //   }
// //   // 4. Actieve ventilatie / circulatie
// //   else if (kasAdvice == GREENHOUSE_VENTILATE) {
// //     if (fanExt1Pct > 0) {
// //       statusText = "💨 Afvoeren " + String(fanExt1Pct) + "%";
// //     } else {
// //       statusText = "🔒 Kas gesloten";
// //     }
// //   } 
// //   else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || fanIntPct > 0) {
// //     statusText = "🌀 Circuleren " + String(fanIntPct) + "%";
// //   } 
// //   else if (isHeatMatRecommended) {
// //     statusText = "🔥 Verwarmingsmat aan";
// //   }
// //   else if (moldRisk) {
// //     statusText = "⚠️ Schimmelrisico";
// //   }

// //   if (mustBlink && !blinkState) {
// //     statusText = ""; 
// //   }

// //   display.setFont(ArialMT_Plain_10);
// //   display.drawString(0, 52, statusText);

// //   display.display();
// // }