/**
 * @file HardwareControl.cpp
 * @brief Implementatie van hardware-aansturing voor 3 ventilatoren (1 intern, 2 extern), knopinteractie, nachtmodus, tachometer en kalibratie-overrule.
 */

#include "HardwareControl.h"
#include "Config.h"
#include "Logger.h"
#include "Telegram.h"
#include "Display.h"
#include <WiFi.h>

// Externe variabelen uit andere modules
extern float indoorSmoothedHum;
extern float kasSmoothedHum;
extern KasVentState kasAdvice;
extern bool isClimateCrash;
extern bool isCalibrationModeActive; // Gedeelde vlag voor kalibratiemodus

// Globale regeltanden (lampen) voor MQTT / Grafana
bool regMoldActive = false;
bool regExtVentActive = false;
bool regIntCircActive = false;
bool regHeatMatActive = false;

// Interrupt Service Routines voor de tachometers
void IRAM_ATTR countIntTacho()  { rpmCountInt++; }
void IRAM_ATTR countExt1Tacho() { rpmCountExt1++; }
void IRAM_ATTR countExt2Tacho() { rpmCountExt2++; }

/**
 * @brief Beheert de status LED op basis van de WiFi-verbinding.
 */
void handleWifiStatusLed() {
  static bool pinInit = false;
  if (!pinInit) {
    pinMode(PIN_NEOPIXEL, OUTPUT);
    pinInit = true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiLedBlink >= 300) {
      lastWifiLedBlink = millis();
      wifiLedState = !wifiLedState; 
      
      if (wifiLedState) {
        digitalWrite(PIN_NEOPIXEL, HIGH); 
      } else {
        digitalWrite(PIN_NEOPIXEL, LOW);    
      }
    }
  } else {
    digitalWrite(PIN_NEOPIXEL, LOW);
    wifiLedState = HIGH;
  }
}

/**
 * @brief Beheert de fysieke BOOT-knop: Schakelt de kas-regeling (winterslaap) stil aan/uit.
 */
void checkFlashButton() {
  static unsigned long lastDebounceTime = 0;
  static bool lastButtonState = HIGH; 
  const unsigned long DEBOUNCE_DELAY = 250;

  int currentReading = digitalRead(FLASH_BUTTON_PIN);

  if (currentReading == LOW && lastButtonState == HIGH) {
    if (millis() - lastDebounceTime > DEBOUNCE_DELAY) {
      lastDebounceTime = millis();
      isKasSleeping = !isKasSleeping;

      if (isKasSleeping) {
        logToSyslogAndSerial("[BUTTON] Boot-knop ingedrukt: Kas in WINTERSLAAP.");
        fanIntSpeed = 0;
        fanExt1Speed = 0;
        fanExt2Speed = 0;
        ledcWrite(PIN_FAN_INT_PWM, 0);
        ledcWrite(PIN_FAN_EXT1_PWM, 0);
        ledcWrite(PIN_FAN_EXT2_PWM, 0);
      } else {
        logToSyslogAndSerial("[BUTTON] Boot-knop ingedrukt: Kas weer ACTIEF.");
      }
    }
  }
  lastButtonState = currentReading;
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
 * @brief Initialiseert de PWM-kanalen en pinnen voor de ventilatoren.
 */
void setupFans() {
  pinMode(PIN_FAN_INT_PWM, OUTPUT);
  pinMode(PIN_FAN_EXT1_PWM, OUTPUT);
  pinMode(PIN_FAN_EXT2_PWM, OUTPUT);

  pinMode(PIN_FAN_INT_TACHO, INPUT_PULLUP);
  pinMode(PIN_FAN_EXT1_TACHO, INPUT_PULLUP);
  pinMode(PIN_FAN_EXT2_TACHO, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(PIN_FAN_INT_TACHO), countIntTacho, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_FAN_EXT1_TACHO), countExt1Tacho, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_FAN_EXT2_TACHO), countExt2Tacho, FALLING);

  ledcAttach(PIN_FAN_INT_PWM, PWM_FREQ, 8);  
  ledcAttach(PIN_FAN_EXT1_PWM, PWM_FREQ, 8);
  ledcAttach(PIN_FAN_EXT2_PWM, PWM_FREQ, 8);

  logToSyslogAndSerial("[HARDWARE] Ventilator pinnen, PWM en Tacho interrupts geconfigureerd.");
}

/**
 * @brief Berekent de RPM voor de fans met versoepelde ruisfilter.
 */
void calculateRPM() {
  static unsigned long lastRpmCalcTime = 0;
  unsigned long currentMillis = millis();
  unsigned long interval = currentMillis - lastRpmCalcTime;
  
  if (interval >= 1000) { 
    if (interval == 0) interval = 1000;

    noInterrupts();
    unsigned long pInt  = rpmCountInt;
    unsigned long pExt1 = rpmCountExt1;
    unsigned long pExt2 = rpmCountExt2;
    rpmCountInt = 0;
    rpmCountExt1 = 0;
    rpmCountExt2 = 0;
    interrupts();

    unsigned long rawIntRPM  = (pInt * 30UL * 1000UL) / interval;
    unsigned long rawExt1RPM = (pExt1 * 30UL * 1000UL) / interval;
    unsigned long rawExt2RPM = (pExt2 * 30UL * 1000UL) / interval;

    const unsigned long MAX_REAL_INT_RPM = 4500;
    const unsigned long MAX_REAL_EXT_RPM = 2500; 

    if (rawIntRPM <= MAX_REAL_INT_RPM)  fanIntRPM  = rawIntRPM;
    if (rawExt1RPM <= MAX_REAL_EXT_RPM) fanExt1RPM = rawExt1RPM;
    if (rawExt2RPM <= MAX_REAL_EXT_RPM) fanExt2RPM = rawExt2RPM;

    if (fanIntSpeed <= 0)  { fanIntRPM = 0; }
    if (fanExt1Speed <= 0) { fanExt1RPM = 0; }
    if (fanExt2Speed <= 0) { fanExt2RPM = 0; }

    if (fanIntRPM < 100)  fanIntRPM = 0;
    if (fanExt1RPM < 100) fanExt1RPM = 0;
    if (fanExt2RPM < 100) fanExt2RPM = 0;

    lastRpmCalcTime = currentMillis;
  }
}

/**
 * @brief Schakelt alle 3 de ventilatoren op hun minimale PWM om lage toerentallen te testen.
 */
void setFansToMinTest() {
  fanIntSpeed  = INT_FAN_BASE_PWM;  
  fanExt1Speed = EXT_FAN_MIN_PWM;   
  fanExt2Speed = EXT_FAN_MIN_PWM;   

  fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
  fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

  ledcWrite(PIN_FAN_INT_PWM,  fanIntSpeed);
  ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
  ledcWrite(PIN_FAN_EXT2_PWM, fanExt2Speed);

  logToSyslogAndSerial("[TEST] Low RPM Test: Alle 3 fans geforceerd op MINIMUM PWM.");
}

/**
 * @brief Laat alle 3 de fans traploos en vloeiend van MIN_PWM naar MAX_PWM rampen.
 */
void updateFanSweepTest() {
  static unsigned long lastSweepUpdate = 0;
  unsigned long currentMillis = millis();

  if (currentMillis - lastSweepUpdate < 50) return;
  lastSweepUpdate = currentMillis;

  unsigned long elapsed = currentMillis - testModeStartTime;
  unsigned long cycleTime = elapsed % 30000; 
  int pwmVal = 0;

  if (cycleTime < 15000) {
    pwmVal = map(cycleTime, 0, 15000, EXT_FAN_MIN_PWM, FAN_MAX_PWM);
  } else {
    pwmVal = map(cycleTime - 15000, 0, 15000, FAN_MAX_PWM, EXT_FAN_MIN_PWM);
  }

  fanIntSpeed  = pwmVal;
  fanExt1Speed = pwmVal;
  fanExt2Speed = pwmVal;

  fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
  fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

  ledcWrite(PIN_FAN_INT_PWM,  fanIntSpeed);
  ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
  ledcWrite(PIN_FAN_EXT2_PWM, fanExt2Speed);
}

/**
 * @brief Regelt de ventilator-snelheden dynamisch op basis van VPD-overschrijding en kas-advies met geleidelijke ramping.
 */
void updateFanSpeeds(float currentVpd) {
  unsigned long currentMillis = millis();

  // --- KALIBRATIEMODUS HARD OVERRULE ---
  if (isCalibrationModeActive) {
    fanIntSpeed  = 0;
    fanExt1Speed = 0;
    fanExt2Speed = 0;
    fanIntPct    = 0;
    fanExt1Pct   = 0;
    fanExt2Pct   = 0;

    ledcWrite(PIN_FAN_INT_PWM,  0);
    ledcWrite(PIN_FAN_EXT1_PWM, 0);
    ledcWrite(PIN_FAN_EXT2_PWM, 0);
    return;
  }

  // --- TFM MANUAL OVERRIDE ---
  if (mfOverrideActive) {
    fanIntSpeed  = 0;          
    fanExt1Speed = FAN_MAX_PWM; 
    fanExt2Speed = FAN_MAX_PWM; 
    fanIntPct    = 0;
    fanExt1Pct   = 100;
    fanExt2Pct   = 100;
    
    ledcWrite(PIN_FAN_INT_PWM,  0);
    ledcWrite(PIN_FAN_EXT1_PWM, FAN_MAX_PWM);
    ledcWrite(PIN_FAN_EXT2_PWM, FAN_MAX_PWM);
    return; 
  }

  // --- KAS WINTERSLAAP CHECK ---
  if (isKasSleeping) {
    fanIntSpeed  = 0;
    fanExt1Speed = 0;
    fanExt2Speed = 0;
    fanIntPct    = 0;
    fanExt1Pct   = 0;
    fanExt2Pct   = 0;
    
    ledcWrite(PIN_FAN_INT_PWM,  0);
    ledcWrite(PIN_FAN_EXT1_PWM, 0);
    ledcWrite(PIN_FAN_EXT2_PWM, 0);
    
    regMoldActive    = false;
    regExtVentActive = false;
    regIntCircActive = false;
    regHeatMatActive = false;
    return; 
  }

  // --- TESTMODUS OVERRIDE ---
  if (isTestModeActive) {
    if (currentMillis - testModeStartTime > currentTestDuration) {
      isTestModeActive = false;
      logToSyslogAndSerial("[TESTMODE] Testmodus automatisch beëindigd.");
    } else {
      if (currentTestDuration == 300000) {
        updateFanSweepTest();
      } 
      else if (currentTestDuration == 30000 && fanExt1Speed == FAN_MAX_PWM) {
        fanIntSpeed  = FAN_MAX_PWM;
        fanExt1Speed = FAN_MAX_PWM;
        fanExt2Speed = FAN_MAX_PWM;
        fanIntPct    = 100;
        fanExt1Pct   = 100;
        fanExt2Pct   = 100;

        ledcWrite(PIN_FAN_INT_PWM, FAN_MAX_PWM);
        ledcWrite(PIN_FAN_EXT1_PWM, FAN_MAX_PWM);
        ledcWrite(PIN_FAN_EXT2_PWM, FAN_MAX_PWM);
      }
      return; 
    }
  }

  if (currentMillis - lastPidTime < 200) return; 
  lastPidTime = currentMillis;
  yield();

  int targetIntSpeed  = 0;
  int targetExt1Speed = 0;
  int targetExt2Speed = 0;

  if (kasAdvice == GREENHOUSE_VENTILATE) {
    int extPwm = EXT_FAN_BASE_PWM;
    float vpdError = currentVpd - VPD_MAX_OPTIMAL;

    if (vpdError > 0.0f) {
      int calculatedPwm = map((int)(vpdError * 100.0f), 0, 10, EXT_FAN_BASE_PWM, FAN_MAX_PWM);
      extPwm = constrain(calculatedPwm, EXT_FAN_BASE_PWM, FAN_MAX_PWM);
    } else {
      extPwm = EXT_FAN_BASE_PWM;
    }

    if (isClimateCrash || kasSmoothedTemp >= GREENHOUSE_CRASH_TEMP) {
      extPwm = FAN_MAX_PWM;
    }

    extPwm = constrain(extPwm, EXT_FAN_MIN_PWM, FAN_MAX_PWM);

    targetExt1Speed = extPwm;
    targetExt2Speed = extPwm; 
    targetIntSpeed  = 0;  
  }
  else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
    targetExt1Speed = 0;
    targetExt2Speed = 0;

    float vpdError = fabs(currentVpd - pidSetPointVPD);

    if (lampsAreOn || vpdError > TEMP_HYSTERESIS_THRESHOLD) {
      int dynamicBoost = map((int)(vpdError * 100.0f), 0, 30, 0, (FAN_MAX_PWM - INT_FAN_BASE_PWM));
      targetIntSpeed = constrain(INT_FAN_BASE_PWM + dynamicBoost, INT_FAN_BASE_PWM, FAN_MAX_PWM);
    } else {
      targetIntSpeed = INT_FAN_MIN_PWM;
    }
  }
  else {
    targetIntSpeed = INT_FAN_MIN_PWM;
    targetExt1Speed = 0;
    targetExt2Speed = 0;
  }

  // --- GELEIDELIJKE RAMPING (SLEW RATE LIMITING) ---
  // Voorkomt abrupte sprongen (het "hoppa"-effect) door de snelheid in kleine stapjes (max 5) bij te stellen
  const int maxStep = 5;
  
  if (fanIntSpeed < targetIntSpeed) fanIntSpeed = min((int)targetIntSpeed, fanIntSpeed + maxStep);
  else if (fanIntSpeed > targetIntSpeed) fanIntSpeed = max((int)targetIntSpeed, fanIntSpeed - maxStep);

  if (fanExt1Speed < targetExt1Speed) fanExt1Speed = min((int)targetExt1Speed, fanExt1Speed + maxStep);
  else if (fanExt1Speed > targetExt1Speed) fanExt1Speed = max((int)targetExt1Speed, fanExt1Speed - maxStep);

  if (fanExt2Speed < targetExt2Speed) fanExt2Speed = min((int)targetExt2Speed, fanExt2Speed + maxStep);
  else if (fanExt2Speed > targetExt2Speed) fanExt2Speed = max((int)targetExt2Speed, fanExt2Speed - maxStep);

  fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
  fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

  regMoldActive    = moldRisk;
  regExtVentActive = (kasAdvice == GREENHOUSE_VENTILATE && fanExt1Speed > 0);
  regIntCircActive = (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL && fanIntSpeed > 0);
  regHeatMatActive = isHeatMatRecommended;

  ledcWrite(PIN_FAN_INT_PWM,  fanIntSpeed);
  ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
  ledcWrite(PIN_FAN_EXT2_PWM, fanExt2Speed);
}
// /**
//  * @file HardwareControl.cpp
//  * @brief Implementatie van hardware-aansturing voor 3 ventilatoren (1 intern, 2 extern), knopinteractie, nachtmodus, tachometer en kalibratie-overrule.
//  */

// #include "HardwareControl.h"
// #include "Config.h"
// #include "Logger.h"
// #include "Telegram.h"
// #include "Display.h"
// #include <WiFi.h>

// // Externe variabelen uit andere modules
// extern float indoorSmoothedHum;
// extern float kasSmoothedHum;
// extern KasVentState kasAdvice;
// extern bool isClimateCrash;
// extern bool isCalibrationModeActive; // Gedeelde vlag voor kalibratiemodus

// // Globale regeltanden (lampen) voor MQTT / Grafana
// bool regMoldActive = false;
// bool regExtVentActive = false;
// bool regIntCircActive = false;
// bool regHeatMatActive = false;

// // Interrupt Service Routines voor de tachometers
// void IRAM_ATTR countIntTacho()  { rpmCountInt++; }
// void IRAM_ATTR countExt1Tacho() { rpmCountExt1++; }
// void IRAM_ATTR countExt2Tacho() { rpmCountExt2++; }

// /**
//  * @brief Beheert de status LED op basis van de WiFi-verbinding.
//  */
// void handleWifiStatusLed() {
//   static bool pinInit = false;
//   if (!pinInit) {
//     pinMode(PIN_NEOPIXEL, OUTPUT);
//     pinInit = true;
//   }

//   if (WiFi.status() != WL_CONNECTED) {
//     if (millis() - lastWifiLedBlink >= 300) {
//       lastWifiLedBlink = millis();
//       wifiLedState = !wifiLedState; 
      
//       if (wifiLedState) {
//         digitalWrite(PIN_NEOPIXEL, HIGH); 
//       } else {
//         digitalWrite(PIN_NEOPIXEL, LOW);    
//       }
//     }
//   } else {
//     digitalWrite(PIN_NEOPIXEL, LOW);
//     wifiLedState = HIGH;
//   }
// }

// /**
//  * @brief Beheert de fysieke BOOT-knop: Schakelt de kas-regeling (winterslaap) stil aan/uit.
//  */
// void checkFlashButton() {
//   static unsigned long lastDebounceTime = 0;
//   static bool lastButtonState = HIGH; 
//   const unsigned long DEBOUNCE_DELAY = 250;

//   int currentReading = digitalRead(FLASH_BUTTON_PIN);

//   if (currentReading == LOW && lastButtonState == HIGH) {
//     if (millis() - lastDebounceTime > DEBOUNCE_DELAY) {
//       lastDebounceTime = millis();
//       isKasSleeping = !isKasSleeping;

//       if (isKasSleeping) {
//         logToSyslogAndSerial("[BUTTON] Boot-knop ingedrukt: Kas in WINTERSLAAP.");
//         fanIntSpeed = 0;
//         fanExt1Speed = 0;
//         fanExt2Speed = 0;
//         ledcWrite(PIN_FAN_INT_PWM, 0);
//         ledcWrite(PIN_FAN_EXT1_PWM, 0);
//         ledcWrite(PIN_FAN_EXT2_PWM, 0);
//       } else {
//         logToSyslogAndSerial("[BUTTON] Boot-knop ingedrukt: Kas weer ACTIEF.");
//       }
//     }
//   }
//   lastButtonState = currentReading;
// }

// /**
//  * @brief Regelt de nachtmodus (display dimmen of uitzetten bij duisternis).
//  */
// void handleNightMode(float currentLux) {
//   const float DARK_THRESHOLD = 1.0;             
//   const unsigned long NIGHT_MODE_DELAY = 5000;  

//   if (currentLux <= DARK_THRESHOLD) {
//     if (!isDisplayOff) {
//       if (darkStartTime == 0) {
//         darkStartTime = millis(); 
//       } else if (millis() - darkStartTime >= NIGHT_MODE_DELAY) {
//         logToSyslogAndSerial("[OLED] Nachtmodus: Scherm UIT");
//         display.displayOff(); 
//         isDisplayOff = true;
//       }
//     }
//   } else {
//     darkStartTime = 0;
//     if (isDisplayOff) {
//       logToSyslogAndSerial("[OLED] Licht gedetecteerd: Scherm AAN");
//       display.displayOn(); 
//       isDisplayOff = false;
//     }
//     if (currentLux <= 1) display.setBrightness(15);  
//     else if (currentLux < 4) display.setBrightness(100); 
//     else display.setBrightness(155);  
//   }
// }

// /**
//  * @brief Houdt bij of er stabiele veranderingen in het lichtniveau zijn.
//  */
// void checkLuxStability(float currentLux) {
//   if (stableLuxBase < 0) {
//     stableLuxBase = currentLux;
//     lastLightStateOn = (currentLux >= 1.8);
//     return;
//   }

//   bool isSignificantlyDifferent = (abs(currentLux - stableLuxBase) > 25.0);

//   if (isSignificantlyDifferent) {
//     if (!isLuxShiftPending) {
//       isLuxShiftPending = true;
//       luxShiftStartTime = millis();
//     } else if (millis() - luxShiftStartTime >= LUX_STABILITY_TIMEOUT) {
//       bool currentLightStateOn = (currentLux > 30.0);
//       if (currentLightStateOn != lastLightStateOn) {
//         sendTelegramAlert(currentLightStateOn ? "💡 *Licht AANGEGAAN in de kas*" : "🌑 *Licht UITGEGAAN in de kas*");
//         lastLightStateOn = currentLightStateOn;
//       }
//       stableLuxBase = currentLux;
//       isLuxShiftPending = false;
//     }
//   } else {
//     isLuxShiftPending = false;
//     stableLuxBase = (stableLuxBase * 0.95) + (currentLux * 0.05);
//   }
// }

// /**
//  * @brief Initialiseert de PWM-kanalen en pinnen voor de ventilatoren.
//  */
// void setupFans() {
//   pinMode(PIN_FAN_INT_PWM, OUTPUT);
//   pinMode(PIN_FAN_EXT1_PWM, OUTPUT);
//   pinMode(PIN_FAN_EXT2_PWM, OUTPUT);

//   pinMode(PIN_FAN_INT_TACHO, INPUT_PULLUP);
//   pinMode(PIN_FAN_EXT1_TACHO, INPUT_PULLUP);
//   pinMode(PIN_FAN_EXT2_TACHO, INPUT_PULLUP);

//   attachInterrupt(digitalPinToInterrupt(PIN_FAN_INT_TACHO), countIntTacho, FALLING);
//   attachInterrupt(digitalPinToInterrupt(PIN_FAN_EXT1_TACHO), countExt1Tacho, FALLING);
//   attachInterrupt(digitalPinToInterrupt(PIN_FAN_EXT2_TACHO), countExt2Tacho, FALLING);

//   ledcAttach(PIN_FAN_INT_PWM, PWM_FREQ, 8);  
//   ledcAttach(PIN_FAN_EXT1_PWM, PWM_FREQ, 8);
//   ledcAttach(PIN_FAN_EXT2_PWM, PWM_FREQ, 8);

//   logToSyslogAndSerial("[HARDWARE] Ventilator pinnen, PWM en Tacho interrupts geconfigureerd.");
// }

// /**
//  * @brief Berekent de RPM voor de fans met versoepelde ruisfilter.
//  */
// void calculateRPM() {
//   static unsigned long lastRpmCalcTime = 0;
//   unsigned long currentMillis = millis();
//   unsigned long interval = currentMillis - lastRpmCalcTime;
  
//   if (interval >= 1000) { 
//     if (interval == 0) interval = 1000;

//     noInterrupts();
//     unsigned long pInt  = rpmCountInt;
//     unsigned long pExt1 = rpmCountExt1;
//     unsigned long pExt2 = rpmCountExt2;
//     rpmCountInt = 0;
//     rpmCountExt1 = 0;
//     rpmCountExt2 = 0;
//     interrupts();

//     unsigned long rawIntRPM  = (pInt * 30UL * 1000UL) / interval;
//     unsigned long rawExt1RPM = (pExt1 * 30UL * 1000UL) / interval;
//     unsigned long rawExt2RPM = (pExt2 * 30UL * 1000UL) / interval;

//     const unsigned long MAX_REAL_INT_RPM = 4500;
//     const unsigned long MAX_REAL_EXT_RPM = 2500; 

//     if (rawIntRPM <= MAX_REAL_INT_RPM)  fanIntRPM  = rawIntRPM;
//     if (rawExt1RPM <= MAX_REAL_EXT_RPM) fanExt1RPM = rawExt1RPM;
//     if (rawExt2RPM <= MAX_REAL_EXT_RPM) fanExt2RPM = rawExt2RPM;

//     if (fanIntSpeed <= 0)  { fanIntRPM = 0; }
//     if (fanExt1Speed <= 0) { fanExt1RPM = 0; }
//     if (fanExt2Speed <= 0) { fanExt2RPM = 0; }

//     if (fanIntRPM < 100)  fanIntRPM = 0;
//     if (fanExt1RPM < 100) fanExt1RPM = 0;
//     if (fanExt2RPM < 100) fanExt2RPM = 0;

//     lastRpmCalcTime = currentMillis;
//   }
// }

// /**
//  * @brief Schakelt alle 3 de ventilatoren op hun minimale PWM om lage toerentallen te testen.
//  */
// void setFansToMinTest() {
//   fanIntSpeed  = INT_FAN_BASE_PWM;  
//   fanExt1Speed = EXT_FAN_MIN_PWM;   
//   fanExt2Speed = EXT_FAN_MIN_PWM;   

//   fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
//   fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
//   fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

//   ledcWrite(PIN_FAN_INT_PWM,  fanIntSpeed);
//   ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
//   ledcWrite(PIN_FAN_EXT2_PWM, fanExt2Speed);

//   logToSyslogAndSerial("[TEST] Low RPM Test: Alle 3 fans geforceerd op MINIMUM PWM.");
// }

// /**
//  * @brief Laat alle 3 de fans traploos en vloeiend van MIN_PWM naar MAX_PWM rampen.
//  */
// void updateFanSweepTest() {
//   static unsigned long lastSweepUpdate = 0;
//   unsigned long currentMillis = millis();

//   if (currentMillis - lastSweepUpdate < 50) return;
//   lastSweepUpdate = currentMillis;

//   unsigned long elapsed = currentMillis - testModeStartTime;
//   unsigned long cycleTime = elapsed % 30000; 
//   int pwmVal = 0;

//   if (cycleTime < 15000) {
//     pwmVal = map(cycleTime, 0, 15000, EXT_FAN_MIN_PWM, FAN_MAX_PWM);
//   } else {
//     pwmVal = map(cycleTime - 15000, 0, 15000, FAN_MAX_PWM, EXT_FAN_MIN_PWM);
//   }

//   fanIntSpeed  = pwmVal;
//   fanExt1Speed = pwmVal;
//   fanExt2Speed = pwmVal;

//   fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
//   fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
//   fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

//   ledcWrite(PIN_FAN_INT_PWM,  fanIntSpeed);
//   ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
//   ledcWrite(PIN_FAN_EXT2_PWM, fanExt2Speed);
// }

// /**
//  * @brief Regelt de ventilator-snelheden dynamisch op basis van VPD-overschrijding en kas-advies.
//  */
// void updateFanSpeeds(float currentVpd) {
//   unsigned long currentMillis = millis();

//   // --- KALIBRATIEMODUS HARD OVERRULE ---
//   if (isCalibrationModeActive) {
//     fanIntSpeed  = 0;
//     fanExt1Speed = 0;
//     fanExt2Speed = 0;
//     fanIntPct    = 0;
//     fanExt1Pct   = 0;
//     fanExt2Pct   = 0;

//     ledcWrite(PIN_FAN_INT_PWM,  0);
//     ledcWrite(PIN_FAN_EXT1_PWM, 0);
//     ledcWrite(PIN_FAN_EXT2_PWM, 0);
//     return;
//   }

//   // --- TFM MANUAL OVERRIDE ---
//   if (mfOverrideActive) {
//     fanIntSpeed  = 0;          
//     fanExt1Speed = FAN_MAX_PWM; 
//     fanExt2Speed = FAN_MAX_PWM; 
//     fanIntPct    = 0;
//     fanExt1Pct   = 100;
//     fanExt2Pct   = 100;
    
//     ledcWrite(PIN_FAN_INT_PWM,  0);
//     ledcWrite(PIN_FAN_EXT1_PWM, FAN_MAX_PWM);
//     ledcWrite(PIN_FAN_EXT2_PWM, FAN_MAX_PWM);
//     return; 
//   }

//   // --- KAS WINTERSLAAP CHECK ---
//   if (isKasSleeping) {
//     fanIntSpeed  = 0;
//     fanExt1Speed = 0;
//     fanExt2Speed = 0;
//     fanIntPct    = 0;
//     fanExt1Pct   = 0;
//     fanExt2Pct   = 0;
    
//     ledcWrite(PIN_FAN_INT_PWM,  0);
//     ledcWrite(PIN_FAN_EXT1_PWM, 0);
//     ledcWrite(PIN_FAN_EXT2_PWM, 0);
    
//     regMoldActive    = false;
//     regExtVentActive = false;
//     regIntCircActive = false;
//     regHeatMatActive = false;
//     return; 
//   }

//   // --- TESTMODUS OVERRIDE ---
//   if (isTestModeActive) {
//     if (currentMillis - testModeStartTime > currentTestDuration) {
//       isTestModeActive = false;
//       logToSyslogAndSerial("[TESTMODE] Testmodus automatisch beëindigd.");
//     } else {
//       if (currentTestDuration == 300000) {
//         updateFanSweepTest();
//       } 
//       else if (currentTestDuration == 30000 && fanExt1Speed == FAN_MAX_PWM) {
//         fanIntSpeed  = FAN_MAX_PWM;
//         fanExt1Speed = FAN_MAX_PWM;
//         fanExt2Speed = FAN_MAX_PWM;
//         fanIntPct    = 100;
//         fanExt1Pct   = 100;
//         fanExt2Pct   = 100;

//         ledcWrite(PIN_FAN_INT_PWM, FAN_MAX_PWM);
//         ledcWrite(PIN_FAN_EXT1_PWM, FAN_MAX_PWM);
//         ledcWrite(PIN_FAN_EXT2_PWM, FAN_MAX_PWM);
//       }
//       return; 
//     }
//   }

//   if (currentMillis - lastPidTime < 200) return; 
//   lastPidTime = currentMillis;
//   yield();

//   int targetIntSpeed  = 0;
//   int targetExt1Speed = 0;
//   int targetExt2Speed = 0;

//   if (kasAdvice == GREENHOUSE_VENTILATE) {
//     static unsigned long fanStartTime = 0;
//     static bool kickActive = false;

//     if (fanExt1Speed == 0 && !kickActive) {
//       fanStartTime = currentMillis;
//       kickActive = true;
//     }

//     int extPwm = EXT_FAN_BASE_PWM;

//     if (kickActive && (currentMillis - fanStartTime < 1000)) {
//       extPwm = FAN_MAX_PWM; 
//     } 
//     else {
//       kickActive = false; 

//       // AGRESSIEVERE VPD CURVE: Snellere opschaling naar max PWM bij stijgende VPD
//       float vpdError = currentVpd - VPD_MAX_OPTIMAL;

//       if (vpdError > 0.0f) {
//         int calculatedPwm = map((int)(vpdError * 150.0f), 0, 10, EXT_FAN_BASE_PWM, FAN_MAX_PWM);
//         extPwm = constrain(calculatedPwm, EXT_FAN_BASE_PWM, FAN_MAX_PWM);
//       } else {
//         extPwm = EXT_FAN_BASE_PWM;
//       }
//     }

//     if (isClimateCrash || kasSmoothedTemp >= GREENHOUSE_CRASH_TEMP) {
//       extPwm = FAN_MAX_PWM;
//     }

//     extPwm = constrain(extPwm, EXT_FAN_MIN_PWM, FAN_MAX_PWM);

//     targetExt1Speed = extPwm;
//     targetExt2Speed = extPwm; 
//     targetIntSpeed  = 0;  
//   }
//   else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
//     targetExt1Speed = 0;
//     targetExt2Speed = 0;

//     // DYNAMISCHE INTERNE FAN STURING MET HARDE ONDERGRENS
//     float vpdError = fabs(currentVpd - pidSetPointVPD);

//     if (lampsAreOn || vpdError > TEMP_HYSTERESIS_THRESHOLD) {
//       int dynamicBoost = map((int)(vpdError * 100.0f), 0, 30, 0, (FAN_MAX_PWM - INT_FAN_BASE_PWM));
//       targetIntSpeed = constrain(INT_FAN_BASE_PWM + dynamicBoost, INT_FAN_BASE_PWM, FAN_MAX_PWM);
//     } else {
//       // Zelfs in rust / standaard circulatie Altijd minimaal draaien op INT_FAN_MIN_PWM!
//       targetIntSpeed = INT_FAN_MIN_PWM;
//     }
//   }
//   else {
//     // Als de kas in een andere lege/off status staat, ook dan minimaal de basisbescherming aanhouden
//     targetIntSpeed = INT_FAN_MIN_PWM;
//     targetExt1Speed = 0;
//     targetExt2Speed = 0;
//   }
//   // else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
//   //   targetExt1Speed = 0;
//   //   targetExt2Speed = 0;

//   //   // Dynamische sturing voor de interne ventilator tijdens gesloten stand:
//   //   float vpdError = fabs(currentVpd - pidSetPointVPD);

//   //   // Als de lampen aan staan of de VPD afwijkt van het setpoint (0.70 kPa)
//   //   if (lampsAreOn || vpdError > TEMP_HYSTERESIS_THRESHOLD) {
//   //     // Schaal traploos op van INT_FAN_BASE_PWM naar FAN_MAX_PWM op basis van de afwijking
//   //     int dynamicBoost = map((int)(vpdError * 100.0f), 0, 30, 0, (FAN_MAX_PWM - INT_FAN_BASE_PWM));
//   //     targetIntSpeed = constrain(INT_FAN_BASE_PWM + dynamicBoost, INT_FAN_BASE_PWM, FAN_MAX_PWM);
//   //   } else {
//   //     targetIntSpeed = INT_FAN_BASE_PWM;
//   //   }
//   // }
//   // // else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
//   // //   targetIntSpeed  = INT_FAN_BASE_PWM;
//   // //   targetExt1Speed = 0;
//   // //   targetExt2Speed = 0;
//   // // }
//   // else {
//   //   targetIntSpeed  = 0;
//   //   targetExt1Speed = 0;
//   //   targetExt2Speed = 0;
//   // }

//   // --- HARDWARE KEIHARDE FAILSAFE ---
//   if (targetExt1Speed > 0 || targetExt2Speed > 0 || kasAdvice == GREENHOUSE_VENTILATE) {
//     targetIntSpeed = 0;
//   }

//   fanIntSpeed  = targetIntSpeed;
//   fanExt1Speed = targetExt1Speed;
//   fanExt2Speed = targetExt2Speed;

//   fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
//   fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
//   fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

//   regMoldActive    = moldRisk;
//   regExtVentActive = (kasAdvice == GREENHOUSE_VENTILATE && fanExt1Speed > 0);
//   regIntCircActive = (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL && fanIntSpeed > 0);
//   regHeatMatActive = isHeatMatRecommended;

//   ledcWrite(PIN_FAN_INT_PWM,  fanIntSpeed);
//   ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
//   ledcWrite(PIN_FAN_EXT2_PWM, fanExt2Speed);
// }