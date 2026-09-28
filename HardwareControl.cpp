/**
 * @file HardwareControl.cpp
 * @brief Implementatie van hardware-aansturing voor 3 ventilatoren (1 intern, 2 extern) met PID, kickstart en tacho (ESP32-S3).
 */

#include <Arduino.h>
#include "Config.h"
#include "Logger.h"
#include "Telegram.h"
#include "Display.h"
#include <WiFi.h>

// --- EXTERN DECLARATIES VOOR GLOBALE VARIABELEN ---
extern bool isTestModeActive;
extern unsigned long testModeStartTime;
extern unsigned long currentTestDuration;

extern unsigned long lastPidTime;
extern float kp, ki, kd;
extern float pError, iError, dError, lastError;
extern float pidSetPointVPD;
extern float pidOutput;

extern bool moldRisk;
extern float kasSmoothedHum;

extern int fanIntSpeed, fanIntRPM;
extern int fanExt1Speed, fanExt1RPM;
extern int fanExt2Speed, fanExt2RPM;

extern unsigned long lastWifiLedBlink;
extern bool wifiLedState;
extern unsigned long lastButtonPressTime;
extern bool pendingTelegramAlert;

extern bool isDisplayOff;
extern unsigned long darkStartTime;
extern float stableLuxBase;
extern bool lastLightStateOn;
extern bool isLuxShiftPending;
extern unsigned long luxShiftStartTime;

// Externe functie
String buildStatusReport();

// Interrupt tellers voor de tachometers
void IRAM_ATTR tachoIntISR()  { rpmCountInt++; }
void IRAM_ATTR tachoExt1ISR() { rpmCountExt1++; }
void IRAM_ATTR tachoExt2ISR() { rpmCountExt2++; }

/**
 * @brief Initialiseert de ventilator pinnen, PWM (LEDC) kanalen en tacho interrupts.
 */
void setupFans() {
  pinMode(PIN_FAN_INT_PWM, OUTPUT);
  pinMode(PIN_FAN_EXT1_PWM, OUTPUT);
  pinMode(PIN_FAN_EXT2_PWM, OUTPUT);

  // ESP32 LEDC configuratie voor 25 kHz (geschikt voor 4-pin PWM ventilatoren zoals Noctua)
  ledcAttach(PIN_FAN_INT_PWM, 25000, 8);  // 8-bit resolutie (0 - 255)
  ledcAttach(PIN_FAN_EXT1_PWM, 25000, 8);
  ledcAttach(PIN_FAN_EXT2_PWM, 25000, 8);

  // Geen INPUT_PULLUP meer ivm hardware pull-up weerstand
  // pinMode(PIN_FAN_INT_TACHO, INPUT);
  // pinMode(PIN_FAN_EXT1_TACHO, INPUT);
  // pinMode(PIN_FAN_EXT2_TACHO, INPUT);
  pinMode(PIN_FAN_INT_TACHO, INPUT_PULLUP);
  pinMode(PIN_FAN_EXT1_TACHO, INPUT_PULLUP);
  pinMode(PIN_FAN_EXT2_TACHO, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(PIN_FAN_INT_TACHO), tachoIntISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_FAN_EXT1_TACHO), tachoExt1ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_FAN_EXT2_TACHO), tachoExt2ISR, FALLING);
}

/**
 * @brief Beheert de status LED op basis van de WiFi-verbinding.
 */
void handleWifiStatusLed() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiLedBlink >= 300) {
      lastWifiLedBlink = millis();
      wifiLedState = !wifiLedState; 
      #ifdef LED_BUILTIN
      if (wifiLedState) analogWrite(LED_BUILTIN, 128); 
      else digitalWrite(LED_BUILTIN, HIGH); 
      #endif
    }
  } else {
    #ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, HIGH);
    #endif
    wifiLedState = HIGH;
  }
}

/**
 * @brief Controleert of de flash-knop wordt ingedrukt voor een statusmelding.
 */
void checkFlashButton() {
  if (digitalRead(FLASH_BUTTON_PIN) == LOW) {
    if (millis() - lastButtonPressTime > BUTTON_DEBOUNCE_DELAY) {
      lastButtonPressTime = millis();
      pendingTelegramAlert = true; 
      logToSyslogAndSerial("[BUTTON] Flash-knop ingedrukt, statusrapport aangevraagd.");
    }
  }
}

/**
 * @brief Regelt de nachtmodus (display dimmen of uitzetten bij duisternis).
 */
void handleNightMode(float currentLux) {
  const float DARK_THRESHOLD = 1.0;             
  const unsigned long NIGHT_MODE_DELAY = 5000;  

  if (currentLux <= DARK_THRESHOLD) {
    if (!isDisplayOff) {
      if (darkStartTime == 0) {
        darkStartTime = millis(); 
      } else if (millis() - darkStartTime >= NIGHT_MODE_DELAY) {
        logToSyslogAndSerial("[OLED] Nachtmodus: Scherm UIT");
        display.displayOff(); 
        isDisplayOff = true;
      }
    }
  } else {
    darkStartTime = 0;
    if (isDisplayOff) {
      logToSyslogAndSerial("[OLED] Licht gedetecteerd: Scherm AAN");
      display.displayOn(); 
      isDisplayOff = false;
    }
    if (currentLux <= 1) display.setBrightness(15);  
    else if (currentLux < 4) display.setBrightness(100); 
    else display.setBrightness(155);  
  }
}

/**
 * @brief Houdt bij of er stabiele veranderingen in het lichtniveau zijn.
 */
void checkLuxStability(float currentLux) {
  if (stableLuxBase < 0) {
    stableLuxBase = currentLux;
    lastLightStateOn = (currentLux >= 1.8);
    return;
  }

  bool isSignificantlyDifferent = (abs(currentLux - stableLuxBase) > 25.0);

  if (isSignificantlyDifferent) {
    if (!isLuxShiftPending) {
      isLuxShiftPending = true;
      luxShiftStartTime = millis();
    } else if (millis() - luxShiftStartTime >= LUX_STABILITY_TIMEOUT) {
      bool currentLightStateOn = (currentLux > 30.0);
      if (currentLightStateOn != lastLightStateOn) {
        sendTelegramAlert(currentLightStateOn ? "💡 *Licht AANGEGAAN in de kas*" : "🌑 *Licht UITGEGAAN in de kas*");
        lastLightStateOn = currentLightStateOn;
      }
      stableLuxBase = currentLux;
      isLuxShiftPending = false;
    }
  } else {
    isLuxShiftPending = false;
      stableLuxBase = (stableLuxBase * 0.95) + (currentLux * 0.05);
  }
}

/**
 * @brief Stuurt de 3 ventilatoren aan volgens een trapsgewijze cascade-regeling op basis van VPD en PID.
 */
void updateFanSpeeds(float currentVpd) {
  unsigned long currentMillis = millis();

  // --- 1. TESTMODUS OVERRIDE ---
  if (isTestModeActive) {
    if (currentMillis - testModeStartTime > currentTestDuration) {
      isTestModeActive = false;
      logToSyslogAndSerial("[TESTMODE] Testmodus automatisch beëindigd.");
    } else {
      ledcWrite(PIN_FAN_INT_PWM, 255); 
      ledcWrite(PIN_FAN_EXT1_PWM, 255);
      ledcWrite(PIN_FAN_EXT2_PWM, 255);
      fanIntSpeed  = 100; 
      fanExt1Speed = 100;
      fanExt2Speed = 100;
      return; 
    }
  }

  int targetIntPwm  = 0;
  int targetExt1Pwm = 0;
  int targetExt2Pwm = 0;

  // Wachttijd voor externe fans tegen "dansen" (bijv. 2 minuten = 120000 ms)
  const unsigned long EXT_FAN_STARTUP_DELAY = 120000; 

  // --- 2. OPSTARTFASE (Eerste 2 minuten) ---
  if (currentMillis < EXT_FAN_STARTUP_DELAY) {
    // Interne fan DRAAIT DIRECT voor constante circulatie
    targetIntPwm  = INT_FAN_BASE_PWM; 
    // Externe fans dicht tijdens opstarten
    targetExt1Pwm = 0;
    targetExt2Pwm = 0;
  } 
  else {
    // --- 3. NORMALE REGELING NA OPSTARTFASE ---
    if (currentMillis - lastPidTime < 2000) return; 
    yield(); 

    float dt = (currentMillis - lastPidTime) / 1000.0;
    lastPidTime = currentMillis;

    float error = pidSetPointVPD - currentVpd;

    pError = error;
    iError += error * dt;
    iError = constrain(iError, -5.0, 5.0);

    dError = (dt > 0) ? (error - lastError) / dt : 0;
    lastError = error;

    pidOutput = (kp * pError) + (ki * iError) + (kd * dError);

    // --- 4. STURING OP BASIS VAN KAS-ADVIES & CASCADE LOGICA ---
    if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || kasAdvice == OFF) {
      // In rust / alleen circulatie: Interne fan AAN, externe fans UIT
      targetIntPwm  = INT_FAN_BASE_PWM;
      targetExt1Pwm = 0;
      targetExt2Pwm = 0;
    } 
    else if (kasAdvice == GREENHOUSE_VENTILATE) {
      // Zodra externe fans moeten ventileren, gaat de interne fan UIT (tegen elkaar in blazen voorkomen)
      targetIntPwm = 0; 

      // --- TRAP 1: EXTERN 1 (Hoofd-ventilator) ---
      // PID output vertalen naar PWM voor Ext 1
      int ext1Pwm = EXT_FAN_BASE_PWM + (int)(pidOutput * 50.0);
      
      if (ext1Pwm < EXT_FAN_MIN_PWM) {
        targetExt1Pwm = 0; 
      } else {
        targetExt1Pwm = constrain(ext1Pwm, EXT_FAN_MIN_PWM, 255);
      }

      // --- TRAP 2: EXTERN 2 (Boost-ventilator / Cascade) ---
      // Als er schimmelrisico is, of als Ext 1 al op volle toeren draait (> 200 PWM) maar de VPD nog te hoog is, 
      // of de LV is nog te hoog (> 80%), dan springt Ext 2 in en schaalt deze mee op basis van de PID-output!
      if (moldRisk || kasSmoothedHum > 80.0 || targetExt1Pwm >= 220) {
        // Bereken vermogen voor Ext 2, start vanaf minimum of schaal mee met de vraag
        int ext2Pwm = EXT_FAN_BASE_PWM + (int)((pidOutput - 2.0) * 60.0); // Bouwt op naarmate de druk toeneemt
        
        if (moldRisk || kasSmoothedHum > 80.0) {
          targetExt2Pwm = 255; // Noodgeval / schimmelrisico = direct volle kracht
        } else if (ext2Pwm < EXT_FAN_MIN_PWM) {
          targetExt2Pwm = EXT_FAN_MIN_PWM; // Anders minimaal aan
        } else {
          targetExt2Pwm = constrain(ext2Pwm, EXT_FAN_MIN_PWM, 255);
        }
      } else {
        targetExt2Pwm = 0; // Geen boost nodig, Ext 1 redt het in eentje
      }
    }
  }

  // --- 5. DUBBELE CHECK VOOR DE INTERNE FAN ---
  if (targetIntPwm > 0 && targetIntPwm < INT_FAN_MIN_PWM) {
    targetIntPwm = INT_FAN_MIN_PWM;
  }

  // --- 6. KICKSTART LOGICA ---
  static unsigned long kickstartStartTime = 0;
  static bool isKickstarting = false;
  static int lastTotalDemand = 0;

  int totalDemand = targetIntPwm + targetExt1Pwm + targetExt2Pwm;
  if (lastTotalDemand == 0 && totalDemand > 0) {
    isKickstarting = true;
    kickstartStartTime = currentMillis;
    logToSyslogAndSerial("[FAN] Kickstart geactiveerd!");
  }
  lastTotalDemand = totalDemand;

  int finalIntPwm  = targetIntPwm;
  int finalExt1Pwm = targetExt1Pwm;
  int finalExt2Pwm = targetExt2Pwm;

  if (isKickstarting) {
    if (currentMillis - kickstartStartTime < 1200) { 
      if (finalIntPwm > 0)  finalIntPwm  = 255;
      if (finalExt1Pwm > 0) finalExt1Pwm = 255;
      if (finalExt2Pwm > 0) finalExt2Pwm = 255;
    } else {
      isKickstarting = false; 
    }
  }

  // --- 7. OMZETTEN NAAR PERCENTAGES VOOR MQTT/TELEGRAM ---
  fanIntSpeed  = map(finalIntPwm,  0, 255, 0, 100);
  fanExt1Speed = map(finalExt1Pwm, 0, 255, 0, 100);
  fanExt2Speed = map(finalExt2Pwm, 0, 255, 0, 100);

  // --- 8. FYSIEKE AANSTURING VIA LEDC ---
  ledcWrite(PIN_FAN_INT_PWM,  finalIntPwm);
  ledcWrite(PIN_FAN_EXT1_PWM, finalExt1Pwm);
  ledcWrite(PIN_FAN_EXT2_PWM, finalExt2Pwm);
}









// /**
//  * @brief Stuurt de 3 ventilatoren aan op basis van VPD, PID, kas-advies en schimmelrisico.
//  */
// void updateFanSpeeds(float currentVpd) {
//   unsigned long currentMillis = millis();

//   // --- 1. TESTMODUS OVERRIDE ---
//   if (isTestModeActive) {
//     if (currentMillis - testModeStartTime > currentTestDuration) {
//       isTestModeActive = false;
//       logToSyslogAndSerial("[TESTMODE] Testmodus automatisch beëindigd.");
//     } else {
//       ledcWrite(PIN_FAN_INT_PWM, 255); 
//       ledcWrite(PIN_FAN_EXT1_PWM, 255);
//       ledcWrite(PIN_FAN_EXT2_PWM, 255);
//       fanIntSpeed  = 100; 
//       fanExt1Speed = 100;
//       fanExt2Speed = 100;
//       return; 
//     }
//   }

//   int targetIntPwm  = 0;
//   int targetExt1Pwm = 0;
//   int targetExt2Pwm = 0;

//   // --- 2. OPSTART- / LEERFASE (Eerste 3 minuten na reboot) ---
//   if (currentMillis < 180000) {
//     // Interne fan circuleert direct voor stabiele metingen, externe fans hard dicht tegen "dansen"
//     targetIntPwm  = CIRCULATION_PWM; 
//     targetExt1Pwm = 0;
//     targetExt2Pwm = 0;
//   } 
//   else {
//     // --- 3. NORMALE REGELING NA OPSTARTEN ---
//     if (currentMillis - lastPidTime < 2000) return; 
//     yield(); 

//     float dt = (currentMillis - lastPidTime) / 1000.0;
//     lastPidTime = currentMillis;

//     float error = pidSetPointVPD - currentVpd;

//     pError = error;
//     iError += error * dt;
//     iError = constrain(iError, -5.0, 5.0);

//     dError = (dt > 0) ? (error - lastError) / dt : 0;
//     lastError = error;

//     pidOutput = (kp * pError) + (ki * iError) + (kd * dError);

//     // --- 4. STURING OP BASIS VAN KAS-ADVIES ---
//     if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || kasAdvice == OFF) {
//       // Systeem in rust of intern circuleren: Interne fan aan, externe fans uit
//       targetIntPwm  = CIRCULATION_PWM;
//       targetExt1Pwm = 0;
//       targetExt2Pwm = 0;
//     } 
//     else if (kasAdvice == GREENHOUSE_VENTILATE) {
//       // Externe fans blazen naar buiten -> Interne fan UIT (tegen elkaar in blazen voorkomen)
//       targetIntPwm = 0; 

//       // Externe ventilator 1 sturing via PID
//       int extPwm = EXT_FAN_BASE_PWM + (int)(pidOutput * 50.0);
      
//       if (extPwm < EXT_FAN_MIN_PWM) {
//         targetExt1Pwm = 0; // Te weinig kracht = veilig uit
//       } else {
//         targetExt1Pwm = constrain(extPwm, EXT_FAN_MIN_PWM, 255);
//       }

//       // Externe ventilator 2 (Boost bij schimmelrisico of hoge LV)
//       if (moldRisk || kasSmoothedHum > 80.0) {
//         targetExt2Pwm = 255; 
//       } else if (extPwm > 160) {
//         targetExt2Pwm = max(extPwm, EXT_FAN_MIN_PWM); 
//       } else {
//         targetExt2Pwm = 0;
//       }
//     }
//   }

//   // --- 5. DUBBELE CHECK VOOR DE INTERNE FAN ---
//   if (targetIntPwm > 0 && targetIntPwm < INT_FAN_MIN_PWM) {
//     targetIntPwm = INT_FAN_MIN_PWM;
//   }

//   // --- 6. KICKSTART LOGICA (Voorkomt vastzitten bij opstarten) ---
//   static unsigned long kickstartStartTime = 0;
//   static bool isKickstarting = false;
//   static int lastTotalDemand = 0;

//   int totalDemand = targetIntPwm + targetExt1Pwm + targetExt2Pwm;
//   if (lastTotalDemand == 0 && totalDemand > 0) {
//     isKickstarting = true;
//     kickstartStartTime = currentMillis;
//     logToSyslogAndSerial("[FAN] Kickstart geactiveerd!");
//   }
//   lastTotalDemand = totalDemand;

//   int finalIntPwm  = targetIntPwm;
//   int finalExt1Pwm = targetExt1Pwm;
//   int finalExt2Pwm = targetExt2Pwm;

//   if (isKickstarting) {
//     if (currentMillis - kickstartStartTime < 1200) { 
//       if (finalIntPwm > 0)  finalIntPwm  = 255;
//       if (finalExt1Pwm > 0) finalExt1Pwm = 255;
//       if (finalExt2Pwm > 0) finalExt2Pwm = 255;
//     } else {
//       isKickstarting = false; 
//     }
//   }

//   // --- 7. OMZETTEN NAAR PERCENTAGES VOOR MQTT/TELEGRAM ---
//   fanIntSpeed  = map(finalIntPwm,  0, 255, 0, 100);
//   fanExt1Speed = map(finalExt1Pwm, 0, 255, 0, 100);
//   fanExt2Speed = map(finalExt2Pwm, 0, 255, 0, 100);

//   // --- 8. FYSIEKE AANSTURING VIA LEDC ---
//   ledcWrite(PIN_FAN_INT_PWM,  finalIntPwm);
//   ledcWrite(PIN_FAN_EXT1_PWM, finalExt1Pwm);
//   ledcWrite(PIN_FAN_EXT2_PWM, finalExt2Pwm);
// }

// /**
//  * @brief Stuurt de 3 ventilatoren aan op basis van VPD, PID, kas-advies en schimmelrisico.
//  */
// void updateFanSpeeds(float currentVpd) {
//   unsigned long currentMillis = millis();

//   // --- 1. TESTMODUS OVERRIDE ---
//   if (isTestModeActive) {
//     if (currentMillis - testModeStartTime > currentTestDuration) {
//       isTestModeActive = false;
//       logToSyslogAndSerial("[TESTMODE] Testmodus automatisch beëindigd.");
//     } else {
//       ledcWrite(PIN_FAN_INT_PWM, 255); 
//       ledcWrite(PIN_FAN_EXT1_PWM, 255);
//       ledcWrite(PIN_FAN_EXT2_PWM, 255);
//       fanIntSpeed  = 100; 
//       fanExt1Speed = 100;
//       fanExt2Speed = 100;
//       return; 
//     }
//   }

//   int targetIntPwm  = 0;
//   int targetExt1Pwm = 0;
//   int targetExt2Pwm = 0;

//   // --- 2. OPSTART- / LEERFASE (Eerste 3 minuten na reboot) ---
//   if (currentMillis < 180000) {
//     // KRUCIAAL: Interne fan DIRECT laten circuleren voor goede meting! Externe fans dicht.
//     targetIntPwm  = CIRCULATION_PWM; 
//     targetExt1Pwm = 0;
//     targetExt2Pwm = 0;
//   } 
//   else {
//     // --- 3. NORMALE REGELING NA OPSTARTEN ---
//     if (currentMillis - lastPidTime < 2000) return; 
//     yield(); 

//     float dt = (currentMillis - lastPidTime) / 1000.0;
//     lastPidTime = currentMillis;

//     float error = pidSetPointVPD - currentVpd;

//     pError = error;
//     iError += error * dt;
//     iError = constrain(iError, -5.0, 5.0);

//     dError = (dt > 0) ? (error - lastError) / dt : 0;
//     lastError = error;

//     pidOutput = (kp * pError) + (ki * iError) + (kd * dError);

//     // --- 4. STURING OP BASIS VAN KAS-ADVIES ---
//     if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || kasAdvice == OFF) {
//       // Systeem in rust, wachten of intern circuleren: Interne fan aan, extern uit
//       targetIntPwm  = CIRCULATION_PWM;
//       targetExt1Pwm = 0;
//       targetExt2Pwm = 0;
//     } 
//     else if (kasAdvice == GREENHOUSE_VENTILATE) {
//       // Externe fans blazen naar buiten -> Interne fan UIT (tegen elkaar in blazen voorkomen)
//       targetIntPwm = 0; 

//       // Externe ventilator 1 sturing via PID
//       int extPwm = EXT_FAN_BASE_PWM + (int)(pidOutput * 50.0);
      
//       // IJZEREN VEILIGHEIDSCHECK: Geen flauwe kul PWM-waardes waar de fan van smoort
//       if (extPwm < EXT_FAN_MIN_PWM) {
//         targetExt1Pwm = 0; // Te weinig kracht = veilig uit
//       } else {
//         targetExt1Pwm = constrain(extPwm, EXT_FAN_MIN_PWM, 255);
//       }

//       // Externe ventilator 2 (Boost bij schimmelrisico of hoge LV)
//       if (moldRisk || kasSmoothedHum > 80.0) {
//         targetExt2Pwm = 255; 
//       } else if (extPwm > 160) {
//         targetExt2Pwm = max(extPwm, EXT_FAN_MIN_PWM); 
//       } else {
//         targetExt2Pwm = 0;
//       }
//     }
//   }

//   // --- 5. DUBBELE CHECK VOOR DE INTERNE FAN (Mag nooit te laag draaien) ---
//   if (targetIntPwm > 0 && targetIntPwm < INT_FAN_MIN_PWM) {
//     targetIntPwm = INT_FAN_MIN_PWM;
//   }

//   // --- 6. KICKSTART LOGICA (Voorkomt vastzitten bij aanzetten) ---
//   static unsigned long kickstartStartTime = 0;
//   static bool isKickstarting = false;
//   static int lastTotalDemand = 0;

//   int totalDemand = targetIntPwm + targetExt1Pwm + targetExt2Pwm;
//   if (lastTotalDemand == 0 && totalDemand > 0) {
//     isKickstarting = true;
//     kickstartStartTime = currentMillis;
//     logToSyslogAndSerial("[FAN] Kickstart geactiveerd!");
//   }
//   lastTotalDemand = totalDemand;

//   int finalIntPwm  = targetIntPwm;
//   int finalExt1Pwm = targetExt1Pwm;
//   int finalExt2Pwm = targetExt2Pwm;

//   if (isKickstarting) {
//     if (currentMillis - kickstartStartTime < 1200) { 
//       if (finalIntPwm > 0)  finalIntPwm  = 255;
//       if (finalExt1Pwm > 0) finalExt1Pwm = 255;
//       if (finalExt2Pwm > 0) finalExt2Pwm = 255;
//     } else {
//       isKickstarting = false; 
//     }
//   }

//   // Omzetten naar percentages (0-100%) voor MQTT/Telegram
//   fanIntSpeed  = map(finalIntPwm,  0, 255, 0, 100);
//   fanExt1Speed = map(finalExt1Pwm, 0, 255, 0, 100);
//   fanExt2Speed = map(finalExt2Pwm, 0, 255, 0, 100);

//   // Fysieke aansturing via LEDC naar de pinnen
//   ledcWrite(PIN_FAN_INT_PWM,  finalIntPwm);
//   ledcWrite(PIN_FAN_EXT1_PWM, finalExt1Pwm);
//   ledcWrite(PIN_FAN_EXT2_PWM, finalExt2Pwm);
// }

// void updateFanSpeeds(float currentVpd) {
//   unsigned long currentMillis = millis();

//   // --- TESTMODUS OVERRIDE ---
//   if (isTestModeActive) {
//     if (currentMillis - testModeStartTime > currentTestDuration) {
//       isTestModeActive = false;
//       logToSyslogAndSerial("[TESTMODE] Testmodus automatisch beëindigd.");
//     } else {
//       ledcWrite(PIN_FAN_INT_PWM, 255); 
//       ledcWrite(PIN_FAN_EXT1_PWM, 255);
//       ledcWrite(PIN_FAN_EXT2_PWM, 255);
//       fanIntSpeed  = 100; 
//       fanExt1Speed = 100;
//       fanExt2Speed = 100;
//       return; 
//     }
//   }

//   // Eerste 3 minuten opstarten: alle fans stilhouden
//   if (currentMillis < 180000) {
//     ledcWrite(PIN_FAN_INT_PWM, 0); 
//     ledcWrite(PIN_FAN_EXT1_PWM, 0);
//     ledcWrite(PIN_FAN_EXT2_PWM, 0);
//     fanIntSpeed  = 0;
//     fanExt1Speed = 0;
//     fanExt2Speed = 0;
//     return;
//   }

//   if (currentMillis - lastPidTime < 2000) return; 
//   yield(); 

//   float dt = (currentMillis - lastPidTime) / 1000.0;
//   lastPidTime = currentMillis;

//   float error = pidSetPointVPD - currentVpd;

//   pError = error;
//   iError += error * dt;
//   iError = constrain(iError, -5.0, 5.0);

//   dError = (dt > 0) ? (error - lastError) / dt : 0;
//   lastError = error;

//   pidOutput = (kp * pError) + (ki * iError) + (kd * dError);

//   int targetIntPwm  = 0;
//   int targetExt1Pwm = 0;
//   int targetExt2Pwm = 0;

//   // const int MIN_FAN_PWM = 65;       // ~25% minimale startdrempel
//   // const int CIRCULATION_PWM = 105;  // ~41% voor fijne, constante interne luchtcirculatie

//   // --- STURING OP BASIS VAN KAS-ADVIES ---
//   if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL || kasAdvice == OFF) {
//     // Interne fan krijgt zijn vaste, veilige circulatie-PWM (die altijd boven de minimumdrempel ligt)
//     targetIntPwm  = CIRCULATION_PWM; // Bijv. 90 (~35%), ruim boven de startdrempel van de kleine fan
//     targetExt1Pwm = 0;
//     targetExt2Pwm = 0;
//   } 
//   else if (kasAdvice == GREENHOUSE_VENTILATE) {
//     targetIntPwm = 0; // Externe fans aan -> Interne fan uit

//     // Berekening externe fan 1
//     int extPwm = EXT_FAN_BASE_PWM + (int)(pidOutput * 50.0);
    
//     // DE VEILIGHEIDSCHECK: Of hij staat uit (0), of hij krijgt minstens de startdrempel!
//     if (extPwm < EXT_FAN_MIN_PWM) {
//       targetExt1Pwm = 0; // Te weinig vermogen? Dan liever helemaal uit dan smoren!
//     } else {
//       targetExt1Pwm = constrain(extPwm, EXT_FAN_MIN_PWM, 255);
//     }

//     // Externe fan 2 (Boost)
//     if (moldRisk || kasSmoothedHum > 80.0) {
//       targetExt2Pwm = 255; 
//     } else if (extPwm > 160) {
//       targetExt2Pwm = max(extPwm, EXT_FAN_MIN_PWM); 
//     } else {
//       targetExt2Pwm = 0; // Uit als er geen boost nodig is
//     }
//   }

//   // --- INTERNE FAN VEILIGHEIDSCHECK ---
//   // Zorg dat de interne fan in de opstart/circulatie-fase ook nooit op een te lage waarde blijft hangen
//   if (targetIntPwm > 0 && targetIntPwm < INT_FAN_MIN_PWM) {
//     targetIntPwm = INT_FAN_MIN_PWM;
//   }

//   // --- KICKSTART LOGICA VOOR NOCTUA FANS ---
//   static unsigned long kickstartStartTime = 0;
//   static bool isKickstarting = false;
//   static int lastTotalDemand = 0;

//   int totalDemand = targetIntPwm + targetExt1Pwm + targetExt2Pwm;
//   if (lastTotalDemand == 0 && totalDemand > 0) {
//     isKickstarting = true;
//     kickstartStartTime = currentMillis;
//     logToSyslogAndSerial("[FAN] Kickstart geactiveerd!");
//   }
//   lastTotalDemand = totalDemand;

//   int finalIntPwm  = targetIntPwm;
//   int finalExt1Pwm = targetExt1Pwm;
//   int finalExt2Pwm = targetExt2Pwm;

//   if (isKickstarting) {
//     if (currentMillis - kickstartStartTime < 1200) { 
//       if (finalIntPwm > 0)  finalIntPwm  = 255;
//       if (finalExt1Pwm > 0) finalExt1Pwm = 255;
//       if (finalExt2Pwm > 0) finalExt2Pwm = 255;
//     } else {
//       isKickstarting = false; 
//     }
//   }

//   // Omzetten naar percentages (0-100%) voor MQTT/Telegram
//   fanIntSpeed  = map(finalIntPwm,  0, 255, 0, 100);
//   fanExt1Speed = map(finalExt1Pwm, 0, 255, 0, 100);
//   fanExt2Speed = map(finalExt2Pwm, 0, 255, 0, 100);

//   // Fysieke aansturing via LEDC
//   ledcWrite(PIN_FAN_INT_PWM,  finalIntPwm);
//   ledcWrite(PIN_FAN_EXT1_PWM, finalExt1Pwm);
//   ledcWrite(PIN_FAN_EXT2_PWM, finalExt2Pwm);
// }

/**
 * @brief Berekent nauwkeurig de RPM voor alle 3 de fans en controleert op blokkades (stalls).
 */
void calculateRPM() {
  static unsigned long lastRpmCalcTime = 0;
  static unsigned long stallStartExt1 = 0;
  static unsigned long stallStartExt2 = 0;
  static unsigned long stallStartInt  = 0;

  unsigned long currentMillis = millis();
  unsigned long interval = currentMillis - lastRpmCalcTime;
  
  if (interval >= 1000) { 
    if (interval == 0) interval = 1000;

    // 1. Haal de pulsen op uit de interrupt en zet de tellers direct op 0
    noInterrupts();
    unsigned long pInt  = rpmCountInt;
    unsigned long pExt1 = rpmCountExt1;
    unsigned long pExt2 = rpmCountExt2;
    rpmCountInt = 0;
    rpmCountExt1 = 0;
    rpmCountExt2 = 0;
    interrupts();

    // 2. Berekening van RPM (Pulsen * 60 seconden / 2 pulsen per omwenteling / tijd)
    // Een typische PC/Noctua fan geeft 2 pulsen per omwenteling, vandaar de factor 30 (60/2).
    fanIntRPM  = (pInt * 30UL * 1000UL) / interval;
    fanExt1RPM = (pExt1 * 30UL * 1000UL) / interval;
    fanExt2RPM = (pExt2 * 30UL * 1000UL) / interval;
    
    // =========================================================================
    // 3. WATERDICHTE RUIS-FILTERS & HARD OP NUL ZETTEN
    // =========================================================================
    
    // Als de fan-snelheid in de software 0% is, MOET het toerental 0 zijn. Geen uitzonderingen.
    if (fanIntSpeed <= 0)  { fanIntRPM = 0; }
    if (fanExt1Speed <= 0) { fanExt1RPM = 0; }
    if (fanExt2Speed <= 0) { fanExt2RPM = 0; }

    // Als de berekende RPM onmogelijk laag is (bijv. minder dan 80 RPM), is het waarschijnlijk ruis -> op 0 zetten
    if (fanIntRPM < 80)  fanIntRPM = 0;
    if (fanExt1RPM < 80) fanExt1RPM = 0;
    if (fanExt2RPM < 80) fanExt2RPM = 0;

    lastRpmCalcTime = currentMillis;

    // --- Stall checks (Alleen waarschuwen als de fan > 15% draait maar de RPM 0 blijft) ---
    if (millis() > 180000 && !isTestModeActive) {
      if (fanExt1Speed > 15 && fanExt1RPM == 0) {
        if (stallStartExt1 == 0) stallStartExt1 = millis();
        else if (millis() - stallStartExt1 > 15000) {
          sendTelegramAlert("🚨 *ALARM:* Externe ventilator 1 krijgt PWM (`" + String(fanExt1Speed) + "%`), maar draait niet (`0 RPM`)!");
          stallStartExt1 = millis();
        }
      } else { stallStartExt1 = 0; }

      if (fanExt2Speed > 15 && fanExt2RPM == 0) {
        if (stallStartExt2 == 0) stallStartExt2 = millis();
        else if (millis() - stallStartExt2 > 15000) {
          sendTelegramAlert("🚨 *ALARM:* Externe ventilator 2 krijgt PWM (`" + String(fanExt2Speed) + "%`), maar draait niet (`0 RPM`)!");
          stallStartExt2 = millis();
        }
      } else { stallStartExt2 = 0; }

      if (fanIntSpeed > 15 && fanIntRPM == 0) {
        if (stallStartInt == 0) stallStartInt = millis();
        else if (millis() - stallStartInt > 15000) {
          sendTelegramAlert("🚨 *ALARM:* Interne circulatieventilator krijgt PWM (`" + String(fanIntSpeed) + "%`), maar draait niet (`0 RPM`)!");
          stallStartInt = millis();
        }
      } else { stallStartInt = 0; }
    }
  }
}