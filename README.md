# ESP32-S3-Zero Microgreens System

- [ESP32-S3-Zero Microgreens System](#esp32-s3-zero-microgreens-system)
  - [🇬🇧 English: Manual \& Documentation](#-english-manual--documentation)
    - [Core Components of the Climate Control System (ClimateLogic.cpp)](#core-components-of-the-climate-control-system-climatelogiccpp)
    - [Environmental Assessment, Weather API \& 3-Climate Chain (Greenhouse vs. Indoor vs. Outside)](#environmental-assessment-weather-api--3-climate-chain-greenhouse-vs-indoor-vs-outside)
    - [Display, PIR Motion Sensor \& Automatic Lux Dimming (Display.cpp)](#display-pir-motion-sensor--automatic-lux-dimming-displaycpp)
    - [Telemetry, InfluxDB v1.6, Syslog \& Telegram Integration](#telemetry-influxdb-v16-syslog--telegram-integration)
      - [Telegram Bot Commands](#telegram-bot-commands)
  - [🇳🇱 Nederlands: Handleiding \& Documentatie](#-nederlands-handleiding--documentatie)
    - [Kerncomponenten van het Klimaatregelsysteem (ClimateLogic.cpp)](#kerncomponenten-van-het-klimaatregelsysteem-climatelogiccpp)
    - [Slimme Omgevingsbeoordeling, Weer-API \& 3-Klimaten Keten (Kas vs. Woning vs. Buiten)](#slimme-omgevingsbeoordeling-weer-api--3-klimaten-keten-kas-vs-woning-vs-buiten)
    - [Display, PIR-bewegingssensor \& Automatische Lux-dimming (Display.cpp)](#display-pir-bewegingssensor--automatische-lux-dimming-displaycpp)
    - [Telemetrie, InfluxDB v1.6, Syslog \& Telegram-interactie](#telemetrie-influxdb-v16-syslog--telegram-interactie)
      - [Telegram Bot Commando's](#telegram-bot-commandos)

## 🇬🇧 English: Manual & Documentation

📖 **Manual: Robust Climate Control & Telemetry Architecture**

This manual describes the setup and operation of the climate control logic, the direct data stream to a Raspberry Pi (Telegraf + InfluxDB v1.6 + Grafana), syslogs, OTA updates, Telnet serial monitoring, and proactive Telegram notifications for the ESP32-S3 microgreens greenhouse. The system is engineered to be fully headless and robust, allowing the ESP32-S3 to stay safely in its breadboard while you actively code from your chair.
LED Status

By prioritizing critical alerts over normal behavior, the LED always accurately indicates system status (from highest to lowest priority):

- **Blue**: Network / Wi-Fi active or connecting / AP Mode.
- **Yellow**: External weather alert / warning (incoming rough or cold weather outside).
- **Orange**: Action required from YOU (as a home actuator, e.g., ajar indoor door or turn on home fan).
- **Green**: ESP manages greenhouse autonomously / everything is optimal.
- **Off**: System in deep rest.

### Core Components of the Climate Control System (ClimateLogic.cpp)

The climate control system consists of independent smart layers:

1. **Cheap Sensor Filter (Moving Average)**: Keeps a buffer of the last 5 minutes (SENSOR_BUFFER_SIZE = 5) to filter out measurement noise and prevent actuator chatter.
2. **Strict VPD Limits**: Hard, consistent thresholds for Vapor Pressure Deficit to prevent drying out or mold.
3. **Anti-Hunt Protection**: Enforces minimum fan run times (MIN_FAN_RUN_TIME = 180000 ms / 3 minutes) to protect hardware.
4. **Proactive Velocity Check**: Monitors the rate of humidity increase (humidityVelocity) to react immediately to sudden spikes.

### Environmental Assessment, Weather API & 3-Climate Chain (Greenhouse vs. Indoor vs. Outside)

When determining ventilation advice and external warnings, the software uses an external Weather API combined with local sensors:

- **Weather API Integration**: Periodically fetches current outdoor measurements (temperature, humidity, and weather conditions). This serves as the baseline to evaluate if outdoor air is suitable for ventilation.

- **Indoor Buffer**: Evaluates whether the indoor space is stable and drier than the greenhouse (indoorIsViable).

- **Outside Air & Warnings**: Checks if outside air is cooler and not excessively humid (outHum < 85.0). In case of incoming rough or cold weather, the system automatically triggers an external weather alert (reflected via the yellow LED and Telegram).

### Display, PIR Motion Sensor & Automatic Lux Dimming (Display.cpp)

The OLED screen and hardware environment are optimized for low-light conditions:

- **PIR Control**: The screen stays off by default to save energy and wakes up immediately upon motion detection (30-second timeout).
- **Lux Dimming**: To prevent the greenhouse from glowing like a lighthouse at night, the lux sensor measures ambient light. If currentLuxValue <= 1.0 (darkness/night or lights off), brightness is automatically dimmed softly (setBrightness(10)). During daytime or when lights are active, brightness ramps up to 100.

### Telemetry, InfluxDB v1.6, Syslog & Telegram Integration

The ESP32-S3 communicates directly with the backend infrastructure and offers powerful tools for remote development:

- **Data Pipeline**: Direct telemetry pipeline to a Raspberry Pi via Telegraf and InfluxDB v1.6, visualized cleanly in Grafana.
- **Remote Debugging & OTA**: Equipped with Syslog, a Telnet Serial Monitor, and OTA updates via an Access Point (/ota), keeping the ESP safely on its breadboard and preventing hardware wear and tear.

#### Telegram Bot Commands

`/st` or `/status` — Direct overview of all sensor values.

`/ad` or `/advies` — Detailed ventilation and mold diagnosis.

`/mm` or `/minmax` — Daily high/low temperatures and humidity extremes.

`/tf` — Test all fans at 100% (30s).

`/tf2` — Test fans 2 (Internal fan OFF, External fans 1 & 2 to max).

`/mf` — Manual TFM override (Toggle ON/OFF: Internal fan OFF, External fans to full blast).

`/sleep` — Puts greenhouse into sleep/rest mode (cultivation OFF, sensors keep reading).

`/tm` / `/ts` — Test heat mat / test mold toggle.

`/hc` — Hardware & sensor check (free RAM, chip temp, error rates).

`/fa` — Toggle fan alerts (mute/unmute).

`/ota` — Start Access Point for wireless updates.

`/rb` — System reboot.

---

## 🇳🇱 Nederlands: Handleiding & Documentatie

📖 **Handleiding: Robuuste Klimaatregeling & Telemetrie-architectuur**

Deze handleiding beschrijft de opzet en werking van de klimaatsturing, de datastroom rechtstreeks naar een Raspberry Pi (Telegraf + InfluxDB v1.6 + Grafana), syslogs, OTA-updates, Telnet serial monitoring en proactieve Telegram-notificaties voor de ESP32-S3 kiemgroenten-kas. Het systeem is ontworpen om volledig 'headless' en robuust te zijn, zodat de ESP32-S3 lekker op zijn breadboard kan blijven liggen terwijl jij vanuit je stoel actief aan het coden bent.
LED Status

Door prioriteiten te stellen (kritieke meldingen boven normaal gedrag), weet de LED altijd feilloos wat te tonen (van hoog naar laag):

- **Blauw**: Netwerk / Wi-Fi actueel of verbinden / AP-modus.
- **Geel**: Extern weer-alarm / waarschuwing (aankomend ruig of koud weer buiten).
- **Oranje**: Actie vereist van JOU (als woning-actuator, bijv. binnendeur op een kier zetten of de woningventilator aanzetten).
- **Groen**: ESP regelt de kas zelfstandig / alles is optimaal.
- **Uit**: Systeem in diepe rust.

### Kerncomponenten van het Klimaatregelsysteem (ClimateLogic.cpp)

Het klimaatregelsysteem is opgedeeld in een aantal slimme, zelfstandige lagen:

   1. **Filter voor Goedkope Sensoren (Voortschrijdend Gemiddelde)**: Houdt een buffer bij van de laatste 5 minuten (SENSOR_BUFFER_SIZE = 5). Dit voorkomt onnodig klapperen van actuatoren door meetruis.
   2. **Vaste, Strenge VPD-grenzen**: Harde, consistente normen voor Vapor Pressure Deficit om uitdroging of schimmel te voorkomen.
   3. A**nti-Hunt Beveiliging**: Dwingt een minimale looptijd af voor ventilatoren (MIN_FAN_RUN_TIME = 180000 ms / 3 minutes) om hardware te ontlasten.
   4. **Proactieve Snelheidsbewaking (Velocity Check)**: Kijkt naar de stijgingssnelheid van de luchtvochtigheid (humidityVelocity) om direct in te grijpen bij plotselinge pieken.

### Slimme Omgevingsbeoordeling, Weer-API & 3-Klimaten Keten (Kas vs. Woning vs. Buiten)

Bij het bepalen van het ventilatieadvies en externe waarschuwingen gebruikt de software een externe weer-API gecombineerd met lokale sensoren:

- **Weer-API Integratie**: Haalt periodiek actuele buitenmetingen (temperatuur, luchtvochtigheid en weersomstandigheden) op. Dit vormt de basis om te beoordelen of buitenlucht geschikt is om in te zetten.

- **Woningbuffer**: De ESP beoordeelt of de woning stabiel en droger is dan de kas (indoorIsViable).

- **Buitenlucht & Waarschuwingen**: Controleert of de buitenlucht koeler is en niet te klam (outHum < 85.0). Bij dreigend ruig of koud weer activeert het systeem automatisch een extern weer-alarm (zichtbaar via de gele LED en Telegram).

### Display, PIR-bewegingssensor & Automatische Lux-dimming (Display.cpp)

Het OLED-scherm en de hardware-omgeving zijn geoptimaliseerd voor het donker:

- **PIR-sturing**: Het scherm staat standaard uit ter energiebesparing en springt direct aan bij beweging (30 seconden timeout).
- **Lux-dimming**: Om te voorkomen dat de kas 's nachts als een bouwlamp oplicht, meet de lux-sensor de omgevingsverlichting. Als currentLuxValue <= 1.0 (donker/nacht of lampen uit), wordt de helderheid automatisch zacht gedimd (setBrightness(10)). Overdag of bij brandende verlichting springt de helderheid naar 100.

### Telemetrie, InfluxDB v1.6, Syslog & Telegram-interactie

De ESP32-S3 communiceert rechtstreeks met de backend en biedt krachtige tools voor development op afstand:

- **Data Pipeline**: Rechtstreekse datastroom naar een Raspberry Pi via Telegraf en InfluxDB v1.6, uitgelezen in Grafana voor strakke grafieken.
- **Remote Debugging & OTA**: Voorzien van Syslog, een Telnet Serial Monitor en OTA-updates via een Access Point (/ota), zodat de ESP lekker op zijn breadboard kan blijven liggen en hardware slijtage minimaal blijft.

#### Telegram Bot Commando's

`/st` of `/status` — Directe weergave van alle sensorwaarden.

`/ad` of `/advies` — Uitgebreid ventilatie- en schimmeladvies.

`/mm `of `/minmax` — Hoogste en laagste dagtemperaturen en LV.

`/tf` — Test alle fans op 100% (30 sec).

`/tf2` — Testfans 2 (Interne fan UIT, Externe fans 1 & 2 naar max).

`/mf` — Handmatige TFM override (Toggle AAN/UIT: Interne fan UIT, Externe fans naar vol gas).

`/sleep` — Zet de kas op ruststand / kweek uit (sensoren meten door).

`/tm` / `/ts` — Test warmtemat / test schimmeltoggle.

`/hc` — Hardware & sensor check (vrij RAM, chip-temp, foutpercentages).

`/fa` — Fan-alarm aan/uit (dempen).

`/ota` — Start Access Point voor draadloze updates.

`/rb` — Systeemherstart.