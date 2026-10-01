/**
 * @file HardwareControl.cpp
 * @brief Implementatie van hardware-aansturing voor 3 ventilatoren (1 intern, 2 extern), knopinteractie, nachtmodus en tachometer.
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
 * @brief Regelt de snelheden van alle 3 de ventilatoren op basis van PID en kasadvies.
 */
void updateFanSpeeds(float currentVpd) {
  unsigned long currentMillis = millis();

  // --- KAS WINTERSLAAP CHECK ---
  if (isKasSleeping) {
    fanIntSpeed  = 0;
    fanExt1Speed = 0;
    fanExt2Speed = 0;
    
    // Percentages ook direct op 0 zetten
    fanIntPct  = 0;
    fanExt1Pct = 0;
    fanExt2Pct = 0;
    
    ledcWrite(PIN_FAN_INT_PWM,  0);
    ledcWrite(PIN_FAN_EXT1_PWM, 0);
    ledcWrite(PIN_FAN_EXT2_PWM, 0);
    
    // Lampen uit
    regMoldActive = false;
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
      fanIntSpeed  = FAN_MAX_PWM;
      fanExt1Speed = FAN_MAX_PWM;
      fanExt2Speed = FAN_MAX_PWM;

      // Percentages op 100% zetten in testmodus
      fanIntPct  = 100;
      fanExt1Pct = 100;
      fanExt2Pct = 100;

      ledcWrite(PIN_FAN_INT_PWM, FAN_MAX_PWM);
      ledcWrite(PIN_FAN_EXT1_PWM, FAN_MAX_PWM);
      ledcWrite(PIN_FAN_EXT2_PWM, FAN_MAX_PWM);
      return; 
    }
  }

  // Voer de PID-berekening elke 2 seconden uit
  if (currentMillis - lastPidTime < 2000) return; 
  
  float dt = (currentMillis - lastPidTime) / 1000.0;
  lastPidTime = currentMillis;

  float error = pidSetPointVPD - currentVpd;

  pError = error;
  iError += error * dt;
  iError = constrain(iError, -5.0, 5.0);

  dError = (dt > 0) ? (error - lastError) / dt : 0;
  lastError = error;

  pidOutput = (kp * pError) + (ki * iError) + (kd * dError);

  int targetIntSpeed = INT_FAN_BASE_PWM; 
  targetIntSpeed = constrain(targetIntSpeed, INT_FAN_MIN_PWM, FAN_MAX_PWM);
  
  int targetExt1Speed = 0;
  int targetExt2Speed = 0;

  // --- STURING OP BASIS VAN KAS-ADVIES & VEILIGHEID ---
  if (kasAdvice == GREENHOUSE_VENTILATE) {
    // Geen remmende PID als we moeten koelen: geef de externe fans direct een flinke boost!
    targetExt1Speed = FAN_MAX_PWM; // Vol vermogen (255) om de hitte snel af te voeren
    targetExt2Speed = FAN_MAX_PWM; 
    targetIntSpeed  = 0;           // Interne ventilator uit tijdens externe ventilatie
  } 
  else if (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL) {
    targetExt1Speed = 0;
    targetExt2Speed = 0;
    // targetIntSpeed blijft op zijn basisniveau
  } 
  else {
    targetExt1Speed = 0;
    targetExt2Speed = 0;
  }

  // --- KICKSTART LOGICA (Kort en mild om uitschieten in RPM te voorkomen) ---
  static unsigned long kickstartStartTime = 0;
  static bool isKickstarting = false;
  static int lastTargetInt = 0;
  static int lastTargetExt1 = 0;

  if ((lastTargetInt == 0 && targetIntSpeed > 0) || (lastTargetExt1 == 0 && targetExt1Speed > 0)) {
    isKickstarting = true;
    kickstartStartTime = currentMillis;
  }

  int finalIntSpeed = targetIntSpeed;
  int finalExt1Speed = targetExt1Speed;
  int finalExt2Speed = targetExt2Speed;

  if (isKickstarting) {
    if (currentMillis - kickstartStartTime < 800) { // Verkort naar 0.8 sec
      if (targetIntSpeed > 0) finalIntSpeed = min(200, (int)FAN_MAX_PWM); // Niet meteen volle 255 maar 200 (voorkomt 4000RPM piek)
      if (targetExt1Speed > 0) finalExt1Speed = min(200, (int)FAN_MAX_PWM);
      if (targetExt2Speed > 0) finalExt2Speed = min(200, (int)FAN_MAX_PWM);
    } else {
      isKickstarting = false; 
    }
  }

  lastTargetInt = targetIntSpeed;
  lastTargetExt1 = targetExt1Speed;

  fanIntSpeed  = finalIntSpeed;
  fanExt1Speed = finalExt1Speed;
  fanExt2Speed = finalExt2Speed;

  // --- PERCENTAGES DIRECT BEREKENEN VOOR DISPLAY, TELEGRAM & MQTT ---
  fanIntPct  = map(fanIntSpeed,  0, 255, 0, 100);
  fanExt1Pct = map(fanExt1Speed, 0, 255, 0, 100);
  fanExt2Pct = map(fanExt2Speed, 0, 255, 0, 100);

  // --- REGELTANDEN BIJWERKEN VOOR GRAFANA / MQTT ---
  regMoldActive    = moldRisk;
  regExtVentActive = (kasAdvice == GREENHOUSE_VENTILATE && fanExt1Speed > 0);
  regIntCircActive = (kasAdvice == GREENHOUSE_CIRCULATE_INTERNAL && fanIntSpeed > 0);
  regHeatMatActive = isHeatMatRecommended;

  // --- FYSIEKE PINNEN AANSTUREN ---
  ledcWrite(PIN_FAN_INT_PWM,  fanIntSpeed);
  ledcWrite(PIN_FAN_EXT1_PWM, fanExt1Speed);
  ledcWrite(PIN_FAN_EXT2_PWM, fanExt2Speed);
}

/**
 * @brief Berekent nauwkeurig de RPM voor alle 3 de fans en controleert op blokkades.c
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

    noInterrupts();
    unsigned long pInt  = rpmCountInt;
    unsigned long pExt1 = rpmCountExt1;
    unsigned long pExt2 = rpmCountExt2;
    rpmCountInt = 0;
    rpmCountExt1 = 0;
    rpmCountExt2 = 0;
    interrupts();

    fanIntRPM  = (pInt * 30UL * 1000UL) / interval;
    fanExt1RPM = (pExt1 * 30UL * 1000UL) / interval;
    fanExt2RPM = (pExt2 * 30UL * 1000UL) / interval;

    if (fanIntSpeed <= 0)  { fanIntRPM = 0; }
    if (fanExt1Speed <= 0) { fanExt1RPM = 0; }
    if (fanExt2Speed <= 0) { fanExt2RPM = 0; }

    if (fanIntRPM < 80)  fanIntRPM = 0;
    if (fanExt1RPM < 80) fanExt1RPM = 0;
    if (fanExt2RPM < 80) fanExt2RPM = 0;

    lastRpmCalcTime = currentMillis;

    if (millis() > 180000 && !isTestModeActive && fanAlertsEnabled) {
      if (fanExt1Speed >= EXT_FAN_MIN_PWM && fanExt1RPM == 0) {
        if (stallStartExt1 == 0) stallStartExt1 = millis();
        else if (millis() - stallStartExt1 > 15000) {
          sendTelegramAlert("🚨 *ALARM:* Externe ventilator 1 draait niet (`0 RPM`)!");
          stallStartExt1 = millis();
        }
      } else { stallStartExt1 = 0; }

      if (fanExt2Speed >= EXT_FAN_MIN_PWM && fanExt2RPM == 0) {
        if (stallStartExt2 == 0) stallStartExt2 = millis();
        else if (millis() - stallStartExt2 > 15000) {
          sendTelegramAlert("🚨 *ALARM:* Externe ventilator 2 draait niet (`0 RPM`)!");
          stallStartExt2 = millis();
        }
      } else { stallStartExt2 = 0; }

      if (fanIntSpeed >= INT_FAN_MIN_PWM && fanIntRPM == 0) {
        if (stallStartInt == 0) stallStartInt = millis();
        else if (millis() - stallStartInt > 15000) {
          sendTelegramAlert("🚨 *ALARM:* Interne ventilator draait niet (`0 RPM`)!");
          stallStartInt = millis();
        }
      } else { stallStartInt = 0; }
    } else {
      stallStartExt1 = 0;
      stallStartExt2 = 0;
      stallStartInt = 0;
    }
  }
}

void toggleFanAlerts(bool enable) {
  fanAlertsEnabled = enable;
  if (fanAlertsEnabled) {
    sendTelegramAlert("🟢 *Ventilator waarschuwingen weer INGESCHAKELD.*");
  } else {
    sendTelegramAlert("🟡 *Ventilator waarschuwingen UITGESCHAKELD.*");
  }
}

bool getFanAlertsStatus() {
  return fanAlertsEnabled;
}