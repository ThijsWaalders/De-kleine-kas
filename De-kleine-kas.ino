/**
 * @file De kleine kas - ESP32-S3
 * @brief Hoofdbestand voor het Klimaat- en Weerstation "De Kleine Kas" op ESP32-S3
 * @details Beheert de initialisatie, sensoren, logica, display en achtergrondverwerking.
 */

/* ============================================================================
   1. CONFIGURATIE & MODULE INBEGRIPPEN
   ============================================================================ */
#include "Config.h"           // Centrale configuratie en extern-declaraties
#include "LedManager.h"       // Status LED beheer
#include "NetworkManager.h"   // Wi-Fi, Telnet, Syslog, OTA
#include "MQTT.h"             // MQTT-client en datastromen naar Home Assistant
#include "HardwareControl.h"  // Ventilator PWM, tacho en knopbediening
#include "IndoorSensors.h"    // Binnen- en kamersensoren
#include "KasSensors.h"       // Kas-sensoren (DHT, BMP180, BH1750)
#include "EspSensors.h"
#include "ClimateLogic.h"     // Klimaatberekeningen, VPD en drempelwaarden
#include "Telegram.h"         // Telegram bot communicatie
#include "OpenWeather.h"      // Weergegevens ophalen via internet
#include "Display.h"          // OLED display aansturing

Preferences preferences;

/* ============================================================================
   2. SETUP FUNCTIE (EENMALIGE INITIALISATIE)
   ============================================================================ */
void setup() {
  setCpuFrequencyMhz(160); // CPU op 160 MHz  ⚡ Minder hitte bij LDO!
  Serial.begin(115200);
  delay(1000);

  // --- SYSLOG INITIALISEREN ---
  setupLogger();

  // Start de interne temperatuursensor
  initEspSensors(); 
  
  // 1. Status LED initialiseren en op blauw zetten (Opstarten)
  statusLed.begin();
  statusLed.setColor(0, 0, 255); 

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // Active Low LED uit
  pinMode(FLASH_BUTTON_PIN, INPUT_PULLUP);

  // 2. Initialiseer I2C en OLED display direct
  Wire.begin(PIN_SDA, PIN_SCL); 
  display.init();
  display.flipScreenVertically();
  drawBootScreen("Sensoren starten...");

  // 3. Initialiseer hardware modules, sensoren én ventilatoren
  setupKasSensors();
  setupIndoorSensors();
  setupFans(); 

  // 4. Bestandssysteem initialiseren (LittleFS voor ESP32)
  drawBootScreen("Bestandssysteem...");
  if (!LittleFS.begin(true)) {
    logToSyslogAndSerial(F("[FS WARNING] LittleFS kon niet worden gestart."));
  } else {
    logToSyslogAndSerial(F("[FS] LittleFS succesvol gestart."));
  }

  // 5. Netwerk starten (Wi-Fi, vast IP, Telnet, Syslog en ArduinoOTA)
  drawBootScreen("Netwerk starten...");
  setupNetwork();
  
  // 7. Telegram SSL instellingen
  if (ENABLE_TELEGRAM) {
    telegramSslClient.setInsecure();
    telegramSslClient.setTimeout(1500); // Snelle timeout
    logToSyslogAndSerial(F("[TELEGRAM] Telegram client geïnitialiseerd."));
  }

  // 8. Controleer of de ESP is herstart doordat het AP te lang aan stond
  if (apTimedOut) {
    if (WiFi.status() == WL_CONNECTED && ENABLE_TELEGRAM) {
      bot.sendMessage(telid, "⚠️ *Waarschuwing:* De AP-modus is automatisch uitgeschakeld vanwege inactiviteit (5 minuten limiet). Het weerstation is herstart naar de normale netwerkmodus.", "Markdown");
    }
    apTimedOut = false;
  }

  // 9. Slaapstand ophalen uit NVS
  preferences.begin("kas_config", true);
  isKasSleeping = preferences.getBool("sleeping", false);
  preferences.end();

  if (isKasSleeping) {
    logToSyslogAndSerial("[SYSTEM] Herstart gedetecteerd: Kas stond in SLAAPSTAND.");
  }
  
  lastSuccessfulNetworkActivity = millis(); 
  
  // Zorg dat de eerste weer-fetch pas over 30 sec gebeurt i.p.v. direct bij boot
  lastWeatherUpdate = millis(); 

  drawBootScreen("Klaar!");
  delay(300);

  statusLed.setColor(0, 0, 0);
}

/* ============================================================================
   3. MAIN LOOP (CONTINU PROCES)
   ============================================================================ */
void loop() {
  handleDisplayPower();
  
  // 1. Altijd als eerste aanroepen voor Netwerk, OTA-updates en Telnet-clients
  handleNetwork(); 

  // Meet de interne temperatuur en check de veiligheid elke 5 seconden
  if (millis() - lastEspTempCheck > 5000) {
    lastEspTempCheck = millis();
    readEspInternalTemp();
    checkEspTemperatureSafety(); 
  }
  
  // Update LED voor status
  statusLed.update(); 

  // 2. Veiligheidscheck geheugenlekken (< 10KB op ESP32)
  freeHeap = ESP.getFreeHeap();
  if (freeHeap < 10000) { 
    logToSyslogAndSerial(F("[CRITICAL] Geheugen te laag (< 10KB), systeem herstart direct..."));
    delay(100);
    ESP.restart();
  }

  isConnected = (WiFi.status() == WL_CONNECTED);
  wifiRSSI = WiFi.RSSI();

  // 3. Directe opstartmelding via Telegram na succesvolle boot en netwerkverbinding
  static bool bootMsgSent = false;
  if (isConnected && !bootMsgSent && ENABLE_TELEGRAM) {
    bootMsgSent = true;
    
    String bootMsg = "🚀 *De kleine kas is opgestart!*\n";
    bootMsg += "• *Versie:* `" + String(FIRMWARE_VERSION) + " (" + String(BUILD_TIMESTAMP) + ")`\n";
    bootMsg += "• *IP-adres:* `" + WiFi.localIP().toString() + "`\n";
    if (currentLuxValue > 1.0) {
      bootMsg += "• *Boot-bypass:* Kweeklampen gedetecteerd, direct actief!\n";
    } else {
      bootMsg += "• *Status:* Kalibratie / opstartfase gestart.\n";
    }
    bootMsg += "• Automatisering en actieve klimaatregeling draaien.";
    
    sendTelegramAlert(bootMsg);
    logToSyslogAndSerial("[SYSTEM] Opstartmelding met versie " + String(FIRMWARE_VERSION) + " verzonden.");
  }
  // static bool bootMsgSent = false;
  // if (isConnected && !bootMsgSent && ENABLE_TELEGRAM) {
  //   bootMsgSent = true;
    
  //   String bootMsg = "🚀 *De kleine kas is opgestart, je code werkt!*\n";
  //   bootMsg += "• *IP-adres:* `" + WiFi.localIP().toString() + "`\n";
  //   if (currentLuxValue > 1.0) {
  //     bootMsg += "• *Boot-bypass:* Kweeklampen gedetecteerd, direct actief!\n";
  //   } else {
  //     bootMsg += "• *Status:* Kalibratie / opstartfase gestart.\n";
  //   }
  //   bootMsg += "• Automatisering en actieve klimaatregeling draaien.";
    
  //   sendTelegramAlert(bootMsg);
  //   logToSyslogAndSerial("[SYSTEM] Opstartmelding verzonden via Telegram.");
  // }

  // 4. MQTT en Telegram verwerking (Achtergrondtaken)
  if (isConnected) {
    handleMqtt(); 
    
    if (ENABLE_TELEGRAM) {
      static unsigned long lastTelegramCheck = 0;
      unsigned long currentTelegramInterval = isKasSleeping ? 10000 : 4000;
      
      if (millis() - lastTelegramCheck >= currentTelegramInterval || lastTelegramCheck == 0) { 
        lastTelegramCheck = millis();
        handleTelegramIncoming();
      }
    }
  }

  if (pendingTelegramAlert) {
    pendingTelegramAlert = false;
    if (isConnected && ENABLE_TELEGRAM) {
      logToSyslogAndSerial(F("[TELEGRAM] Handmatig statusrapport versturen via knop..."));
      sendTelegramAlert(buildStatusReport());
    } else {
      logToSyslogAndSerial(F("[TELEGRAM WARNING] Kan geen statusrapport sturen: Geen Wi-Fi."));
    }
  }

  // 5. Testmodus timer bewaking
  if (isTestModeActive && (millis() - testModeStartTime > currentTestDuration)) {
    isTestModeActive = false;
    sendTelegramAlert("🧪 *Testmodus afgelopen.*\nSysteem draait weer volledig automatisch op basis van sensoren.");
    logToSyslogAndSerial(F("[TESTMODE] Testmodus automatisch uitgeschakeld."));
  }

  // 6. OpenWeather update interval (elke 15 minuten, voor het eerst na 30s)
  if (millis() - lastWeatherUpdate > 900000) {
    lastWeatherUpdate = millis(); 
    if (isConnected) {
      logToSyslogAndSerial("[WEATHER] Actueel weer en voorspelling ophalen...");
      fetchInternetWeather();
      delay(200); 
      fetchWeatherForecast();
    }
  }

  // Snelheidstoename kas controleren
  checkClimateVelocity(kasSmoothedHum);

  // 7. Sensor uitleescyclus (dynamisch: 5s normaal, 15s in slaapstand)
  unsigned long currentSensorInterval = isKasSleeping ? 15000 : 5000;
  static unsigned long lastSensorRead = 0;
  if (millis() - lastSensorRead >= currentSensorInterval || lastSensorRead == 0) {
    lastSensorRead = millis();

    updateKasSensors();
    updateIndoorSensors();
    
    // Kas Logica
    if (kasSmoothedTemp > -15.0 && kasSmoothedTemp < 60.0) {
      if (displayedTemp == -999.0) displayedTemp = kasSmoothedTemp;

      if (millis() - lastTempUpdate >= TEMP_UPDATE_INTERVAL || abs(kasSmoothedTemp - displayedTemp) >= TEMP_HYSTERESIS_THRESHOLD) {
        displayedTemp = kasSmoothedTemp;
        lastTempUpdate = millis();
      }

      if (!isSystemReady()) {
        moldRisk = false;
        moldReasonText = "⏳ Systeem kalibreert / leert...";
        vpdStatusText = " `(⏳ Opstarten)`";
        dpMarginStatusText = " `(⏳ Kalibreren)`";
      } else {
        if (!isTestModeActive) {
          evaluateClimateState(kasSmoothedTemp, kasSmoothedHum);
          updateMoldRiskHistory(moldRisk);
          updateHeatingAdvice(kasSmoothedTemp, outdoorTemp);
        }

        updateVentilationAdvice(kasSmoothedTemp, kasSmoothedHum, kasDewPoint, outdoorTemp, outdoorHumidity, outdoorDewPoint);
      }

      checkKasTrends(kasSmoothedHum, kasSmoothedHum, currentPressure);

      if (kasSmoothedHum < kasLowHum) kasLowHum = kasSmoothedHum;
      if (kasSmoothedHum > kasHighHum) kasHighHum = kasSmoothedHum;
      
      updateFanSpeeds(kasVpd);
      calculateRPM();
    }

    // Woning logica
    if (indoorSmoothedTemp > -15.0 && indoorSmoothedTemp < 60.0) {
      if (indoorSmoothedTemp < indoorTempLow) indoorTempLow = indoorSmoothedTemp;
      if (indoorSmoothedTemp > indoorTempHigh) indoorTempHigh = indoorSmoothedTemp;
      if (indoorSmoothedHum < indoorLowHum) indoorLowHum = indoorSmoothedHum;
      if (indoorSmoothedHum > indoorHighHum) indoorHighHum = indoorSmoothedHum;
    }

    updateHighLow(kasSmoothedTemp);
    updateHumidityHighLow(kasSmoothedHum, indoorSmoothedHum, outdoorHumidity);
  }

  // 8. OLED Display verversen (max 1x per seconde)
  static unsigned long lastDisplayUpdate = 0;
  if (millis() - lastDisplayUpdate >= 1000) {
    lastDisplayUpdate = millis();
    renderDisplay();
  }

  // Adempauze voor de ESP32 RTOS en Watchdog
  delay(1);
}