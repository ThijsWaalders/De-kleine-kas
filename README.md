# ESP32-S3 Microgreens Climate Control System

**Firmware Version:** `v3.2.9`

**Target Hardware:** ESP32-S3-ZERO-N8R8-M

---

## 🇬🇧 English: Manual & Documentation

📖 **Manual: Robust Climate Control, Agronomic Logic & Telemetry Architecture**

This manual describes the technical and agronomic architecture of the ESP32-S3 microgreens greenhouse system ("De kleine kas"). It is engineered to run completely headless, combining strict agronomic safety margins with robust industrial patterns.

### 1. Hardware Architecture, Pinout & System Setup

The brain of the system is an **ESP32-S3-ZERO-N8R8-M** (running at 160 MHz for optimal thermal and power efficiency), communicating via I2C, PWM, and Interrupts.

* **Greenhouse Sensors (SHT31)**: Industrial high-precision I2C sensor measuring temperature, humidity, VPD, and dew point. Features a built-in software-controllable internal heater for sensor recovery and calibration.
* **Indoor Sensors (BME280)**: Measures reference indoor temperature, humidity, and barometric pressure from the living space buffer zone.
* **Light & Motion**: BH1750 ambient light sensor (lux) and a PIR motion sensor (`PIN_PIR` on GPIO 2) for automatic OLED display wakeup and grow light detection.
* **PWM Fans (4-Wire, 3 Units)**:
* `PIN_FAN_INT_PWM` (GPIO 12) & `PIN_FAN_INT_TACHO` (GPIO 13): Internal circulation fan.
* `PIN_FAN_EXT1_PWM` (GPIO 10) & `PIN_FAN_EXT1_TACHO` (GPIO 11): External intake/exhaust fan 1.
* `PIN_FAN_EXT2_PWM` / TACHO: External intake/exhaust fan 2.
* Operating at **25 kHz PWM** to eliminate audible motor hum. Tachometer lines are protected via inline diodes and read via hardware interrupts (`FALLING`).


* **Actuators & Status**: PWM Heat mat recommendation logic and onboard Status Neopixel LED (`PIN_NEOPIXEL` on GPIO 21).

### 2. Microgreens Agronomy: Why VPD, Thresholds & Buffers Matter

Microgreens are extremely sensitive to microclimates in enclosed spaces:

* **VPD (Vapor Pressure Deficit)**: Primary metric for transpiration.
* **Too Low (`< 0.60 kPa`)**: Stagnant, clammy air leading to fungal outbreaks (*Botrytis*, damping-off) within hours.
* **Too High (`> 0.80 kPa`)**: Causes severe root dehydration and stalls growth.


* **Dew Point Margin (`DP_MARGIN_MIN = 2.0°C`)**: Prevents leaf surface temperature from approaching the dew point, avoiding destructive leaf condensation.
* **Indoor Buffer Strategy**: Instead of drawing cold/damp outdoor air directly into a small greenhouse, the system leverages conditioned indoor air from the living room as a stable climate buffer.

### 3. Core Components of the Climate Control System (`ClimateLogic.cpp`)

1. **Moving Average Sensor Filter**: Maintains a sliding buffer (`SENSOR_BUFFER_SIZE = 3`) to eliminate sensor noise and prevent actuator chattering.
2. **Anti-Hunt Protection**: Enforces a minimum fan run time (`MIN_FAN_RUN_TIME = 180000 ms / 3 min`) to protect motor bearings and electronics.
3. **Proactive Humidity Velocity Check**: Monitors rate of humidity rise (`humidityVelocity`). Surges $\ge 3\%$/min above 60% RH trigger instant internal circulation *before* mold risk establishes.
4. **Boot-Bypass & Light Detection**: If ambient light exceeds 1.0 lux during boot (grow lights detected), the standard 3-minute calibration delay is bypassed, immediately activating automated climate control.
5. **NVS State Memory**: Retains sleep mode (`isKasSleeping`) across reboots via ESP32 `Preferences` non-volatile storage.

### 4. Layered Fan Control & Climate Safety

* **Green Zone (Optimal)**: VPD between `0.60` and `0.80` kPa. Climate stable; fans idle.
* **Orange Zone (Soft Warning)**: Minor clamminess. Internal fan circulates softly (~45% PWM) to break the boundary layer.
* **Red Zone (Hard / Critical)**: Significant VPD deviation or condensation risk. External fans scale dynamically up to 100% capacity.
* **Ultra Max (Emergency Crash)**: Triggered if greenhouse temperature exceeds `GREENHOUSE_CRASH_TEMP` (28°C / 30°C). All external fans force 100% duty cycle with an instant Telegram emergency alert.
* **Sleep Mode (`/sl`)**: Climate regulation pauses and fan PWM signals drop to 0%, while sensors continue logging background data.

### 5. Environmental Assessment & 3-Climate Chain

Compares Greenhouse vs. Indoor vs. Outdoor (via OpenWeather API) to dynamically decide whether to vent, circulate, or seal the greenhouse enclosure.

### 6. Display, PIR Motion Sensor & Automatic Lux Dimming

* **PIR Display Control**: OLED display sleeps by default and wakes for 30 seconds upon motion detection.
* **Lux Dimming**: Automatically dims display brightness to `10` when ambient light drops below `1.0` lux at night.

### 7. Telemetry, MQTT, Syslog & Telegram Commands

Integrates with Home Assistant via MQTT (with local LittleFS buffering for offline recovery), Syslog logging, Telnet monitoring, Wireless AP OTA updates, and Telegram bot commands:

* **Status & Reports:**
* `/st` or `status` — Main report (VPD, Temp, RH per location, and active logic).
* `/hc` or `hwcheck` — Detailed hardware check (Free RAM, Chip Temp, sensor error rates, lux, PIR, Fan RPMs).
* `/mm` or `minmax` — Daily min/max temperature & humidity per location.
* `/ad` or `advies` — Detailed cultivation, ventilation, and mold diagnostics.
* `/ni` or `netwerkinfo` — Network status, RSSI, IP, firmware version (`v3.2.9`), and build timestamp.
* `/h`, `help`, or `start` — Displays the command menu.


* **Testing & Overrides:**
* `/tf` — Tests all fans for 30 sec at 100% PWM.
* `/tf2` — Internal fan OFF, external fans 1 & 2 at 100% PWM (30 sec).
* `/mf` — Manual TFM override toggle (internal fan OFF, external fans 100%).
* `/tm` — Manually toggles heat mat state (15 sec test).
* `/ts` — Manually toggles mold risk state (10 min test).
* `/tc` — Simulates 72°C chip temperature security alert.
* `/fa` — Enables or disables (mutes) fan alerts via Telegram.


* **System & Maintenance:**
* `/sl` or `slaapstand` — Toggles sleep mode (state saved to NVS).
* `/cal` or `kalibratie` — Stops all fans and runs an SHT31 sensor heater test (if lights are off).
* `/ota` — Starts local AP mode for wireless OTA firmware updates.
* `/fl` or `flush` — Clears incoming Telegram queue.
* `/rb` or `reboot` — Restarts the ESP32-S3 immediately.
* `/pe` or `paasei` — *[Secret Easter Egg]* Microgreens facts and jokes.



---

## 🇳🇱 Nederlands: Handleiding & Documentatie

📖 **Handleiding: Robuuste Klimaatregeling, Teeltlogica & Telemetrie-architectuur**

Deze handleiding beschrijft de technische en teelttechnische opzet van het ESP32-S3 microgreens-kassysteem ("De kleine kas"). Het is ontworpen om volautomatisch, 'headless' en extreem bedrijfszeker te draaien in huis.

### 1. Hardware-architectuur, Pinout & Systeemopzet

Het hart van de installatie is een **ESP32-S3-ZERO-N8R8-M** (gedraaid op 160 MHz voor optimale warmte- en energiehuishouding), die I2C-sensoren, PWM-aansturingen en interrupts beheert.

* **Kas Sensoren (SHT31)**: Industriële I2C-sensor voor hoge precisie temperatuur-, luchtvochtigheid-, VPD- en dauwpuntmetingen. Uitgerust met een softwarematig aanstuurbare interne heater voor herstel en kalibratie.
* **Woning Sensoren (BME280)**: Meet referentiewaarden voor temperatuur, luchtvochtigheid én luchtdruk in de woning (bufferzone).
* **Licht & Beweging**: BH1750 lichtsensor (lux) en een PIR-bewegingssensor (`PIN_PIR` op GPIO 2) voor automatische display-inschakeling en kweeklamp-detectie.
* **PWM Fans (4-Wire, 3 stuks)**:
* `PIN_FAN_INT_PWM` (GPIO 12) & `PIN_FAN_INT_TACHO` (GPIO 13): Interne circulatieventilator.
* `PIN_FAN_EXT1_PWM` (GPIO 10) & `PIN_FAN_EXT1_TACHO` (GPIO 11): Externe afzuig-/toevoerventilator 1.
* `PIN_FAN_EXT2_PWM` / TACHO: Externe afzuig-/toevoerventilator 2.
* Aangestuurd op **25 kHz PWM** om motorgezoem volledig te elimineren. Tacho-lijnen zijn beveiligd via diodes en worden uitgelezen via hardware-interrupts (`FALLING`).


* **Actuators & Status**: Warmtemat aansturingslogica en onboard Status Neopixel LED (`PIN_NEOPIXEL` op GPIO 21).

### 2. Teeltlogica Kiemgroenten: Waarom VPD, Drempels & Buffers Cruciaal Zijn

Het opkweken van microgreens (kiemgroenten) in een afgesloten ruimte luistert uiterst nauw:

* **VPD (Vapor Pressure Deficit - Dampdrukverschil)**: De graadmeter voor gewasverdamping.
* **Te laag (`< 0.60 kPa`)**: Stilstaande, klamme lucht waarin schimmels (*Botrytis*, kiemrot) binnen enkele uren toeslaan.
* **Te hoog (`> 0.80 kPa`)**: Veroorzaakt snelle uitdroging van de teer gewortelde kiemplanten.


* **Dauwpuntmarge (`DP_MARGIN_MIN = 2.0°C`)**: Voorkomt dat het bladblad doordringt tot het dauwpunt, wat schadelijke condensvorming op het gewas voorkomt.
* **De Woning als Tussenbuffer**: Buitenlucht is vaak te koud of vochtig. Door de **woonkamer** als buffer te gebruiken, ventileert de kas met stabiele en drogere binnenlucht zonder thermische shocks.

### 3. Kerncomponenten van het Klimaatregelsysteem (`ClimateLogic.cpp`)

1. **Sensor Filter (Voortschrijdend Gemiddelde)**: Filtert metingen via een buffer (`SENSOR_BUFFER_SIZE = 3`) om meetruis en zenuwachtig schakelen (*chattering*) van ventilatoren te voorkomen.
2. **Anti-Hunt Beveiliging**: Dwingt een minimale looptijd af (`MIN_FAN_RUN_TIME = 180000 ms / 3 minuten`) bij statusveranderingen van ventilatoren.
3. **Proactieve Snelheidsbewaking (Velocity Check)**: Detecteert een snelle stijging van de luchtvochtigheid (`humidityVelocity`). Een stijging van $\ge 3\%$/minuut boven de 60% LV activeert direct interne circulatie *voordat* schimmelrisico ontstaat.
4. **Boot-Bypass & Lampdetectie**: Als de lichtsensor bij het opstarten meer dan 1.0 lux meet (kweeklampen aan), wordt de standaard kalibratiewachttijd van 3 minuten overgeslagen en start de klimaatregeling direct.
5. **NVS Geheugenopslag**: Onthoudt de slaapstand-status (`isKasSleeping`) na reboots of stroomuitval via de `Preferences` van de ESP32-S3.

### 4. Gelaagde Fan-besturing & Klimaatbeveiliging

* **Groene Zone (Optimaal)**: VPD tussen `0.60` en `0.80` kPa. Klimaat is stabiel; fans staan in rust.
* **Oranje Zone (Soft Waarschuwing)**: Lichte klamheid. Interne fan draait rustig (~45% PWM) om het microklimaat rond de kiemen te breken.
* **Rode Zone (Hard / Kritiek)**: Grotere VPD-afwijking of condensrisico. Externe fans schalen dynamisch op tot 100%.
* **Ultra Max (Noodtoestand / Klimaatcrash)**: Overschrijdt de kas-temperatuur de kritieke grens (`GREENHOUSE_CRASH_TEMP`, bijv. 28°C / 30°C), dan schakelen alle externe fans naar 100% en wordt direct een Telegram-noodalarm verzonden.
* **Slaapstand (`/sl`)**: Schakelt actieve ventilatie uit (PWM naar 0%), terwijl sensoren op de achtergrond data blijven loggen.

### 5. Slimme Omgevingsbeoordeling & 3-Klimaten Keten

Vergelijkt de toestand in de Kas, Woning en Buiten (via OpenWeather API) om te bepalen of ventilatie met woning- of buitenlucht veilig en effectief is.

### 6. Display, PIR-bewegingssensor & Automatische Lux-dimming

* **PIR-sturing**: Het OLED-scherm staat standaard uit en springt aan bij beweging (30 seconden timeout).
* **Lux-dimming**: Dimt het display automatisch naar stand `10` wanneer het omgevingslicht onder `1.0` lux zakt.

### 7. Telemetrie, MQTT, Syslog & Telegram-interactie

Draadloze integratie met Home Assistant via MQTT (inclusief lokale LittleFS-buffering bij netwerkuitval), Syslog logging, Telnet-monitoring, OTA updates en Telegram-commando's:

* **Informatie & Rapporten:**
* `/st` of `status` — Hoofdrapport met actuele VPD, temp, LV en kasactie.
* `/hc` or `hwcheck` — Hardwarecheck (RAM, chip-temp, sensor fout-percentages, lux, PIR, Fan RPMs).
* `/mm` of `minmax` — Min/max waarden van de dag per locatie.
* `/ad` of `advies` — Uitgebreid teelt-, ventilatie- en schimmeladvies.
* `/ni` of `netwerkinfo` — Netwerkstatus, RSSI, IP, firmwareversie (`v3.2.9`) en build-tijdstempel.
* `/h`, `help` of `start` — Toont het commando-menu.


* **Testen & Overrides:**
* `/tf` — Test alle fans gedurende 30 sec op 100% PWM.
* `/tf2` — Interne fan UIT, externe fans 1 & 2 op 100% PWM (30 sec).
* `/mf` — Handmatige TFM-override (interne fan UIT, externe fans 100%).
* `/tm` — Test warmtemat status (15 sec test).
* `/ts` — Test schimmelrisico status (10 min test).
* `/tc` — Simuleert 72°C chiptemperatuur alarm.
* `/fa` — Schakelt fan-alarmeringen via Telegram in of uit.


* **Systeem & Onderhoud:**
* `/sl` of `slaapstand` — Schakelt slaapstand in/uit (status opgeslagen in NVS).
* `/cal` of `kalibratie` — Zet alle fans stil en triggert een SHT31 heater-check (indien lampen uit zijn).
* `/ota` — Start lokaal AP voor draadloze OTA firmware-updates.
* `/fl` of `flush` — Maakt Telegram-wachtrij leeg.
* `/rb` of `reboot` — Herstart de ESP32-S3 direct.
* `/pe` of `paasei` — *[Geheime Easter Egg]* Roterende kiemgroenten-feitjes en humor.