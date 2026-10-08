# ESP32-S3-Zero Microgreens System

* [ESP32-S3-Zero Microgreens System](https://www.google.com/search?q=%23esp32-s3-zero-microgreens-system)
* [🇬🇧 English: Manual & Documentation](https://www.google.com/search?q=%23-english-manual--documentation)
* [1. Hardware Architecture, Pinout & Temporary Setup](https://www.google.com/search?q=%231-hardware-architecture-pinout--temporary-setup)
* [2. Microgreens Agronomy: Why VPD, Thresholds & Buffers Matter](https://www.google.com/search?q=%232-microgreens-agronomy-why-vpd-thresholds--buffers-matter)
* [3. Core Components of the Climate Control System (ClimateLogic.cpp)](https://www.google.com/search?q=%233-core-components-of-the-climate-control-system-climatelogiccpp)
* [4. Layered Fan Control & Climate Safety (Config.h)](https://www.google.com/search?q=%234-layered-fan-control--climate-safety-configh)
* [5. Environmental Assessment, Weather API & 3-Climate Chain](https://www.google.com/search?q=%235-environmental-assessment-weather-api--3-climate-chain)
* [6. Display, PIR Motion Sensor & Automatic Lux Dimming](https://www.google.com/search?q=%236-display-pir-motion-sensor--automatic-lux-dimming)
* [7. Telemetry, InfluxDB v1.6, Syslog & Telegram Integration](https://www.google.com/search?q=%237-telemetry-influxdb-v16-syslog--telegram-integration)
* [Telegram Bot Commands](https://www.google.com/search?q=%23telegram-bot-commands)
* [🇳🇱 Nederlands: Handleiding & Documentatie](https://www.google.com/search?q=%23-nederlands-handleiding--documentatie)
* [1. Hardware-architectuur, Pinout & Tijdelijke Opzet](https://www.google.com/search?q=%231-hardware-architectuur-pinout--tijdelijke-opzet)
* [2. Teeltlogica Kiemgroenten: Waarom VPD, Drempels & Buffers Cruciaal Zijn](https://www.google.com/search?q=%232-teeltlogica-kiemgroenten-waarom-vpd-drempels--buffers-cruciaal-zijn)
* [3. Kerncomponenten van het Klimaatregelsysteem (ClimateLogic.cpp)](https://www.google.com/search?q=%233-kerncomponenten-van-het-klimaatregelsysteem-climatelogiccpp)
* [4. Gelaagde Fan-besturing & Klimaatbeveiliging (Config.h)](https://www.google.com/search?q=%234-gelaagde-fan-besturing--klimaatbeveiliging-configh)
* [5. Slimme Omgevingsbeoordeling, Weer-API & 3-Klimaten Keten](https://www.google.com/search?q=%235-slimme-omgevingsbeoordeling-weer-api--3-klimaten-keten)
* [6. Display, PIR-bewegingssensor & Automatische Lux-dimming](https://www.google.com/search?q=%236-display-pir-bewegingssensor--automatische-lux-dimming)
* [7. Telemetrie, InfluxDB v1.6, Syslog & Telegram-interactie](https://www.google.com/search?q=%237-telemetrie-influxdb-v16-syslog--telegram-interactie)
* [Telegram Bot Commando's](https://www.google.com/search?q=%23telegram-bot-commandos)

---

## 🇬🇧 English: Manual & Documentation

📖 **Manual: Robust Climate Control, Teeltlogica & Telemetry Architecture**

This manual describes the technical and agronomic architecture of the ESP32-S3 microgreens greenhouse system. It is engineered to run completely headless, combining strict agronomic safety margins with robust industrial patterns.

### 1. Hardware Architecture, Pinout & Temporary Setup

The brain of the system is an **ESP32-S3-Zero**, communicating via I2C, PWM, and Interrupts.

* **PWM Fans (4-Wire)**: Connected directly via GPIO (`PIN_FAN_INT_PWM` / `PIN_FAN_EXT1_PWM` / `PIN_FAN_EXT2_PWM`) running at a clean 25 kHz to prevent motor humming. Power (12V/5V) and GND remain permanently connected; speed is modulated entirely via duty cycle (0-255).
* **Tachometer (RPM Feedback)**: To protect the ESP32 while sharing or transitioning hardware lines before migrating to a larger board, tachometer lines are routed via a **diode (stripe facing the fan)**, a pull-up resistor to 3.3V (`INPUT_PULLUP` enabled in software), and read via hardware interrupts (`FALLING`).
* **Pin Mapping (`Config.h`)**:
* `PIN_PIR` (GPIO 2): Motion sensor for display wakeup.
* `PIN_SCL` (GPIO 4) & `PIN_SDA` (GPIO 5): I2C bus (OLED display, BMP085 pressure/temp, BH1750 lux).
* `INDOOR_DHT_PIN` (GPIO 6) & `KAS_DHT_PIN` (GPIO 7): Climate sensors.
* `PIN_FAN_EXT1_PWM` (GPIO 10), `PIN_FAN_EXT1_TACHO` (GPIO 11): External Fan 1.
* `PIN_FAN_INT_PWM` (GPIO 12), `PIN_FAN_INT_TACHO` (GPIO 13): Internal Circulation Fan.
* `PIN_NEOPIXEL` (GPIO 21): Status LED indicator.



### 2. Microgreens Agronomy: Why VPD, Thresholds & Buffers Matter

Microgreens (germinating seeds and young sprouts) are extremely vulnerable in a closed indoor environment:

* **VPD (Vapor Pressure Deficit)**: Measures the drying power of the air. If VPD is too low (`< 0.60 kPa`), the air is stagnant and clammy, making fungal outbreaks (like *Botrytis* or mold) inevitable within hours. If VPD is too high (`> 0.80 kPa`), delicate root systems dry out and growth stalls.
* **Dew Point Margin (`DP_MARGIN_MIN = 2.0°C`)**: If the surface temperature of the seeds drops too close to the dew point, condensation forms directly on the leaves/seeds—a fatal condition for microgreens.
* **The Indoor Buffer Strategy**: Drawing freezing or humid outdoor air directly into a small indoor greenhouse causes massive climate shocks. By using the **living room (indoor space)** as an intermediate buffer, the system can draw stable, conditioned indoor air to correct minor dryness or humidity spikes safely.

### 3. Core Components of the Climate Control System (ClimateLogic.cpp)

1. **Moving Average Sensor Filter**: Keeps a sliding buffer (`SENSOR_BUFFER_SIZE = 3` sampled over time) to eliminate electrical and physical sensor noise, preventing actuator "chattering."
2. **Anti-Hunt Protection**: Enforces a strict minimum run time (`MIN_FAN_RUN_TIME = 180000 ms / 3 min`) to prevent fans from rapidly toggling on and off, protecting bearings and motor drivers.
3. **Proactive Velocity Check**: Monitors the rate of humidity change (`humidityVelocity`). If humidity surges by $\ge 3\%$/min while above 60%, internal circulation triggers instantly *before* mold risk sets in.

### 4. Layered Fan Control & Climate Safety (Config.h)

* **Green Zone (Optimal)**: VPD between `0.60` and `0.80` kPa. Fans idle.
* **Orange Zone (Soft Warning)**: Minor clamminess. Fan 1 runs softly (~45% PWM / 120 speed) to gently stabilize moisture.
* **Red Zone (Hard / Critical)**: Severe VPD deviation or condensation risk. Fan 1 goes full gas (255), and Fan 2 scales dynamically based on severity.
* **Ultra Max (Emergency Crash)**: Triggered if temperature exceeds `GREENHOUSE_CRASH_TEMP` (28°C / 30°C). Both external fans force 100%, and an instant Telegram alert is dispatched.
* **Sleep Mode (`/sleep`)**: Cultivation pauses and PWM signals drop to 0, leaving fans unpowered via logic while sensors continue logging data over time.

### 5. Environmental Assessment, Weather API & 3-Climate Chain

Evaluates Greenhouse vs. Indoor vs. Outdoor using local sensors and an external Weather API to decide whether to vent, circulate, or seal the greenhouse.

### 6. Display, PIR Motion Sensor & Automatic Lux Dimming

* **PIR Control**: OLED screen sleeps by default and wakes for 30 seconds upon motion.
* **Lux Dimming**: Automatically dims screen brightness to `10` when ambient light drops below `1.0` lux at night.

### 7. Telemetry, InfluxDB v1.6, Syslog & Telegram Integration

Direct telemetry pipeline to a Raspberry Pi via Telegraf and InfluxDB v1.6, visualized in Grafana, complemented by Syslog, Telnet monitoring, OTA updates, and a complete Telegram command set including the secret rotation easter egg (`/ea`).

---

## 🇳🇱 Nederlands: Handleiding & Documentatie

📖 **Handleiding: Robuuste Klimaatregeling, Teeltlogica & Telemetrie-architectuur**

Deze handleiding beschrijft de volledige technische en teelttechnische opzet van het ESP32-S3 microgreens-kassysteem. Het is ontworpen om volautomatisch, 'headless' en extreem bedrijfszeker te draaien in huis.

### 1. Hardware-architectuur, Pinout & Tijdelijke Opzet

Het hart van de installatie is een compacte **ESP32-S3-Zero**, die I2C-sensoren, PWM-aansturingen en interrupts aanstuurt.

* **PWM Fans (4-Wire)**: Rechtstreeks aangesloten op de GPIO's (`PIN_FAN_INT_PWM`, etc.) op een stabiele frequentie van **25 kHz** om hinderlijk motorgezoem te voorkomen. Voeding (12V/5V) en massa (GND) blijven permanent verbonden; de snelheid wordt volledig geregeld via het PWM-signaal (0-255).
* **Tacho-feedback met Diodes**: Omdat de hardware-lijnen op een later moment naar een groter bord worden gemigreerd, lopen de tacho-signalen tijdelijk via een **diode (met de streep naar de fan gericht)** en een pull-up naar 3.3V (`INPUT_PULLUP` actief in software). Hierdoor worden stoorsignalen gefilterd en kan de ESP veilig via hardware-interrupts (`FALLING`) de omwentelingen tellen zonder dat de chip oververhit raakt of beschadigd wordt.
* **Pinoverzicht (`Config.h`)**:
* `PIN_PIR` (GPIO 2): Bewegingssensor voor automatische display-activering.
* `PIN_SCL` (GPIO 4) / `PIN_SDA` (GPIO 5): I2C-bus voor de OLED-display, BMP085 barometersensor en BH1750 lichtsensor.
* `INDOOR_DHT_PIN` (GPIO 6) / `KAS_DHT_PIN` (GPIO 7): DHT11 temperatuur- en vochtigheidssensoren voor respectievelijk de woning en de kas.
* `PIN_FAN_EXT1_PWM` (GPIO 10) & `PIN_FAN_EXT1_TACHO` (GPIO 11): Externe ventilator 1.
* `PIN_FAN_INT_PWM` (GPIO 12) & `PIN_FAN_INT_TACHO` (GPIO 13): Interne circulatieventilator.
* `PIN_NEOPIXEL` (GPIO 21): Status-LED voor visuele feedback.



### 2. Teeltlogica Kiemgroenten: Waarom VPD, Drempels & Buffers Cruciaal Zijn

Het opkweken van microgreens (kiemgroenten) in een afgesloten ruimte luistert uiterst nauw:

* **VPD (Vapor Pressure Deficit - Dampdrukverschil)**: Dit is dé graadmeter voor de verdamping.
* Als de VPD te laag is (`< 0.60 kPa`), staat de lucht stil, is het te klam en krijgen schimmels (zoals sporen van *Botrytis* of valse meeldauw) binnen enkele uren vrij spel.
* Als de VPD te hoog wordt (`> 0.80 kPa`), drogen de tere worteltjes en zaadblaadjes direct uit en stopt de groei.


* **Dauwpuntmarge (`DP_MARGIN_MIN = 2.0°C`)**: Als de bladtemperatuur van de kiemgroenten te dicht bij het dauwpunt komt, slaat er direct condens neer op de blaadjes. Dit is funest voor kiemgroenten omdat het verstikking en rot veroorzaakt.
* **De Woning als Tussenbuffer**: Buitenlucht is vaak te koud, te vochtig of wisselvallig om direct een kleine kas mee te ventileren. Door de **woonkamer** als buffer te gebruiken, kan het systeem schone, stabiele en iets drogere binnenlucht inzetten om vochtpieken in de kas op te vangen zonder het microklimaat te shockeren.

### 3. Kerncomponenten van het Klimaatregelsysteem (ClimateLogic.cpp)

1. **Sensor Filter (Voortschrijdend Gemiddelde)**: Slaat metingen op in een buffer (`SENSOR_BUFFER_SIZE = 3`) om meetruis en kleine fluctuaties weg te filteren. Dit voorkomt dat relais of ventilatoren zenuwachtig aan- en uitschakelen (*chattering*).
2. **Anti-Hunt Beveiliging**: Dwingt een minimale looptijd af (`MIN_FAN_RUN_TIME = 180000 ms / 3 minuten`) zodra een ventilator van status verandert. Dit spaart de lagers en de elektronica.
3. **Proactieve Snelheidsbewaking (Velocity Check)**: Berekent de stijgsnelheid van de luchtvochtigheid (`humidityVelocity`). Stijgt de LV explosief met $\ge 3\%$/minuut bij een LV boven de 60%, dan grijpt het systeem direct in met preventieve interne circulatie *voordat* schimmelrisico optreedt.

### 4. Gelaagde Fan-besturing & Klimaatbeveiliging (Config.h)

De aansturing is opgedeeld in vier heldere veiligheidszones:

* **Groene Zone (Optimaal)**: VPD ligt tussen `0.60` en `0.80` kPa. Alles is in balans; fans staan uit.
* **Oranje Zone (Soft Waarschuwing)**: De kas wordt licht klam. Fan 1 draait zachtjes op ca. 45% PWM (`120`) om de lucht subtiel te verversen.
* **Rode Zone (Hard / Kritiek)**: Grotere VPD-afwijking (`> VPD_RED_OFFSET`) of dreigende condens. Fan 1 gaat vol gas (`255`), en Fan 2 schaalt dynamisch in snelheid op naarmate de afwijking groter wordt.
* **Ultra Max (Noodtoestand / Klimaatcrash)**: Overschrijdt de kas-temperatuur de kritieke grens (`GREENHOUSE_CRASH_TEMP`, bijv. 28°C / 30°C), dan dwingen beide externe fans een 100% capaciteit af en wordt onmiddellijk een Telegram-noodalarm verzonden.
* **Slaapstand (`/sleep`)**: Schakelt de kweekfunctie en ventilatie uit door de PWM-signalen op 0 te zetten (de voeding en GND blijven netjes aangesloten zonder warmteontwikkeling op de ESP), terwijl de sensoren doorgaan met loggen voor data-analyse over tijd.

### 5. Slimme Omgevingsbeoordeling, Weer-API & 3-Klimaten Keten

Vergelijkt continu de toestand in de kas, de woning en buiten (via een externe Weer-API) om te bepalen of ventilatie wenselijk is en of er externe weer-alarmen (gele LED / Telegram) uitgestuurd moeten worden.

### 6. Display, PIR-bewegingssensor & Automatische Lux-dimming

* **PIR-sturing**: Het OLED-scherm staat standaard uit om energie te besparen en springt direct aan bij beweging (30 seconden timeout).
* **Lux-dimming**: Meet via de BH1750 lichtsensor of het donker is (`currentLuxValue <= 1.0`). Zo ja, dan wordt de displayhelderheid automatisch gedimd naar een zachte stand (`10`) om te voorkomen dat de kas 's nachts oplicht als een nachtlamp.

### 7. Telemetrie, InfluxDB v1.6, Syslog & Telegram-interactie

Het systeem communiceert direct met een Raspberry Pi via Telegraf en InfluxDB v1.6 voor grafieken in Grafana, ondersteunt Syslog, Telnet-monitoring, OTA-updates en een uitgebreide set Telegram-commando's:

* `/st` / `/status` — Sensoroverzicht.
* `/ad` / `/advies` — Ventilatie- en schimmeldiagnose.
* `/mm` / `/minmax` — Dagelijkse extremen.
* `/tf` / `/tf2` — Ventilatortests.
* `/mf` — Handmatige override toggle.
* `/sleep` — Slaapstand / kweek pauzeren.
* `/tm` / `/ts` — Warmtemat- en schimmeltoggle.
* `/hc` — Hardware- en geheugenstatus.
* `/fa` — Fan-alarmen dempen of aanzetten.
* `/ota` — Wireless firmware updates via AP.
* `/rb` — Systeemherstart.
* `/fl` — Telegram-wachtrij opschonen.
* `/ea` — *[Verborgen Easter Egg]* Roterende lijst met unieke kiemgroenten-feitjes en humor zonder het help-menu te vervuilen.