/**
 * @file Display.cpp
 * @brief Implementatie van het display met strenge scheiding tussen actieve techniek en actie-voor-gebruiker voor ESP32-S3.
 */

#include "Config.h"
#include "ClimateLogic.h"
#include "Display.h"

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
 * @brief Beheert de helderheid op basis van de kasverlichting (1 lux = uit, >1 lux = aan).
 */
void handleDisplayBrightness(float currentLux) {
  const float LIGHT_THRESHOLD = 1.0;          

  if (currentLux <= LIGHT_THRESHOLD) {
    display.setBrightness(3);   // Zo gedimd mogelijk wanneer lampen uit zijn
  } else {
    display.setBrightness(90); // Half dimmend (mooi in balans op schaal 0-255) wanneer lampen aan zijn
  }
}
// /**
//  * @brief Beheert de helderheid op basis van omgevingslicht.
//  */
// void handleDisplayBrightness(float currentLux) {
//   const float DARK_THRESHOLD = 1.0;          

//   if (currentLux <= DARK_THRESHOLD) {
//     display.setBrightness(5); // Minimaal gedimd in het donker
//   } else {
//     display.setBrightness(100); // Rustige helderheid overdag
//   }
// }

void renderDisplay() {
  handleDisplayBrightness(currentLuxValue);

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);

  // --- 1. BOVENSTE ZONE: Buitenweer & Knipperende Waarschuwingen ---
  display.setColor(BLACK);
  display.fillRect(0, 0, 128, 12);
  display.setColor(WHITE);

  bool blinkState = (millis() / 400) % 2 == 0; // Knippert actief (0.4s)

  if (!isConnected) {
    if (blinkState) display.drawString(0, 0, F("⚠️️ GEEN WIFI"));
  } 
  else if (hasOutdoorAlert && blinkState) {
    display.drawString(0, 0, "ALARM: " + weatherDesc);
  } 
  else {
    // Toont buitentemperatuur + luchtvochtigheid in de banner
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

// --- 3. ONDERSTE ZONE: ESP-Automatisering vs. Menselijke Actie ---
  display.drawLine(0, 51, 128, 51);
  
  int fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  int fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);

  String statusText = "🟢 Kas draait zelfstandig";
  bool mustBlink = false; // Alleen true als JIJ (als woning-actuator) iets moet doen

  // 1. Winterslaap check
  if (isKasSleeping) {
    statusText = "💤 KAS IN WINTERSLAAP";
  }
  // 2. Opstartfase
  else if (millis() < 180000) {
    statusText = "⏳ Systeem kalibreert...";
  }
  // 3. KNIPPEREND: JIJ moet handelen (woning raam open)
  else if (houseAdvice == HOUSE_VENTILATE && moldRisk) {
    statusText = "🚨 Zet woningraam open!";
    mustBlink = true;
  }
  // 4. Testmodus weergave
  else if (isTestModeActive) {
    statusText = "⚡ TESTMODUS ACTIEF";
  }
  // 5. Wat doet de ESP automatisch in/voor de kas?
  else if (kasAdvice == GREENHOUSE_VENTILATE) {
    if (fanExt1Pct > 0) {
      statusText = "💨 ESP AFVOEREN (" + String(fanExt1Pct) + "%)";
    } else {
      statusText = "🔒 ESP Wacht/Gesloten";
    }
  } 
  else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || fanIntPct > 0) {
    statusText = "🌀 ESP CIRCULEREN (" + String(fanIntPct) + "%)";
  } 
  else if (isHeatMatRecommended) {
    statusText = "🔥 ESP Verwarmingsmat AAN";
  }
  // 6. Automatische risicobeheersing door ESP
  else if (moldRisk) {
    statusText = "⚠️ Schimmelrisico (ESP regelt)";
  }

  // Als jij actie moet ondernemen, dwingen we het knipperen van de melding af
  if (mustBlink && !blinkState) {
    statusText = ""; 
  }

  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 52, statusText);

  display.display();
}

// /**
//  * @file Display.cpp
//  * @brief Implementatie van het display met strenge scheiding tussen actieve techniek en actie-voor-gebruiker (knipperend) voor ESP32-S3.
//  */

// #include "Config.h"
// #include "ClimateLogic.h"
// #include "Display.h"

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
//  * @brief Beheert de helderheid op basis van omgevingslicht.
//  */
// void handleDisplayBrightness(float currentLux) {
//   const float DARK_THRESHOLD = 1.0;          

//   if (currentLux <= DARK_THRESHOLD) {
//     display.setBrightness(5); // Minimaal gedimd in het donker
//   } else {
//     display.setBrightness(100); // Rustige helderheid overdag
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
//     if (blinkState) display.drawString(0, 0, F("⚠️ GEEN WIFI"));
//   } 
//   else if (hasOutdoorAlert && blinkState) {
//     display.drawString(0, 0, "ALARM: " + weatherDesc);
//   } 
//   else {
//     // Toont buitentemperatuur + luchtvochtigheid in de banner
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

//   // --- 3. ONDERSTE ZONE: Wat doet de techniek vs. Wanneer jij moet handelen ---
//   display.drawLine(0, 51, 128, 51);

//   int f1Pct   = map(fanExt1Speed, 0, 255, 0, 100);
//   int fIntPct = map(fanIntSpeed,  0, 255, 0, 100);
  
//   String statusText = "🟢 Alles in balans";
//   bool mustBlink = false; // Alleen true als JIJ (de mens) actie moet ondernemen

//   // 1. Winterslaap check
//   if (isKasSleeping) {
//     statusText = "💤 KAS IN WINTERSLAAP";
//   }
//   // 2. Opstartfase
//   else if (millis() < 180000) {
//     statusText = "⏳ Opstarten...";
//   }
//   // 2. KNIPPEREND: Jij moet direct raam openzetten
//   else if (houseAdvice == HOUSE_VENTILATE && moldRisk) {
//     statusText = "🚨 Zet raam open!";
//     mustBlink = true;
//   }
//   // 3. Testmodus weergave
//   else if (isTestModeActive) {
//     statusText = "⚡ TESTMODUS (100%)";
//   }
//   // 4. Wat doet de techniek zelf? (Actieve sturing)
//   else if (kasAdvice == GREENHOUSE_VENTILATE) {
//     // Kijken of de huiskamer daadwerkelijk droger is (woning ventileren) of dat we puur afvoeren
//     if (indoorSmoothedHum < (kasSmoothedHum - 1.5)) {
//       statusText = "🏠 WONING VENT (" + String(f1Pct) + "%)";
//     } else {
//       statusText = "💨 KAS AFVOEREN (" + String(f1Pct) + "%)";
//     }
//   } 
//   else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || fIntPct > 0) {
//     statusText = "🌀 INT CIRCULATIE (" + String(fIntPct) + "%)";
//   } 
//   else if (isHeatMatRecommended) {
//     statusText = "🔥 Warmtemat AAN";
//   }
//   // 5. Loslopend risico onder controle
//   else if (moldRisk) {
//     statusText = "⚠️ Schimmelrisico (Regelend)";
//   }

//   // Als jij actie moet ondernemen, dwingen we het knipperen af
//   if (mustBlink && !blinkState) {
//     statusText = ""; 
//   }

//   display.setFont(ArialMT_Plain_10);
//   display.drawString(0, 52, statusText);

//   display.display();
// }
//   // // --- 3. ONDERSTE ZONE: Wat doet de techniek vs. Wanneer jij moet handelen ---
//   // display.drawLine(0, 51, 128, 51);

//   // int f1Pct = map(fanExt1Speed, 0, PWM_RANGE, 0, 100);
//   // int fIntPct = map(fanIntSpeed, 0, PWM_RANGE, 0, 100);
//   // int maxFanPct = max(f1Pct, fIntPct);
  
//   // String statusText = "🟢 Alles in balans";
//   // bool mustBlink = false; // Alleen true als JIJ (de mens) actie moet ondernemen

//   // // 1. Opstartfase
//   // if (millis() < 180000) {
//   //   statusText = "⏳ Opstarten...";
//   // } 
//   // // 2. KNIPPEREND: Jij moet direct raam openzetten
//   // else if (houseAdvice == HOUSE_VENTILATE && moldRisk) {
//   //   statusText = "🚨 Zet raam open!";
//   //   mustBlink = true;
//   // }
//   // // 3. KNIPPEREND: Condensprobleem en geen fans actief
//   // else if (moldRisk && moldReasonText.indexOf("Condensgevaar") >= 0 && maxFanPct == 0) {
//   //   statusText = "🚨 KRITIEK: Geen fans!";
//   //   mustBlink = true;
//   // }
//   // // 4. Wat doet de techniek zelf?
//   // else if (f1Pct > 0 || fIntPct > 0) {
//   //   if (kasAdvice == GREENHOUSE_VENTILATE) {
//   //     if (indoorHum < kasSmoothedHum) {
//   //       statusText = "💨 Naar kamer: " + String(maxFanPct) + "%";
//   //     } else {
//   //       statusText = "💨 Afvoeren: " + String(maxFanPct) + "%";
//   //     }
//   //   } else {
//   //     statusText = "🌀 Circuleren: " + String(fIntPct) + "%";
//   //   }
//   // } 
//   // else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
//   //   statusText = "🌀 Lucht breken";
//   // }
//   // else if (isHeatMatRecommended) {
//   //   statusText = "🔥 Warmtemat AAN";
//   // }
//   // // 5. Loslopend risico onder controle
//   // else if (moldRisk) {
//   //   statusText = "⚠️ Risico (Actief)";
//   // }

//   // // Als jij actie moet ondernemen, dwingen we het knipperen af
//   // if (mustBlink && !blinkState) {
//   //   statusText = ""; 
//   // }

//   // display.setFont(ArialMT_Plain_10);
//   // display.drawString(0, 52, statusText);

//   // display.display();