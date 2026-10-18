# ESP32-S3 Kiemgroente Klimaat Regeling

- [ESP32-S3 Kiemgroente Klimaat Regeling](#esp32-s3-kiemgroente-klimaat-regeling)
  - [🇳🇱 Nederlands](#-nederlands)
    - [Firmware Versie`v3.3.0`](#firmware-versiev330)
    - [Netwerk \& OTA Stabiliteit (De Grote Doorbraak)](#netwerk--ota-stabiliteit-de-grote-doorbraak)
    - [Telegram Bot \& Commando Suite (v3.3.0)](#telegram-bot--commando-suite-v330)
    - [Systeem \& Versiebeheer](#systeem--versiebeheer)
  - [Handleiding \& Documentatie](#handleiding--documentatie)
    - [1. Hardware-architectuur, Pinout \& Systeemopzet](#1-hardware-architectuur-pinout--systeemopzet)
    - [2. Teeltlogica Kiemgroenten: Waarom VPD, Drempels \& Buffers Cruciaal Zijn](#2-teeltlogica-kiemgroenten-waarom-vpd-drempels--buffers-cruciaal-zijn)
    - [3. Kerncomponenten van het Klimaatregelsysteem (`ClimateLogic.cpp`)](#3-kerncomponenten-van-het-klimaatregelsysteem-climatelogiccpp)
    - [4. Gelaagde Fan-besturing \& Klimaatbeveiliging](#4-gelaagde-fan-besturing--klimaatbeveiliging)
    - [5. Slimme Omgevingsbeoordeling \& 3-Klimaten Keten](#5-slimme-omgevingsbeoordeling--3-klimaten-keten)
    - [6. Display, PIR-bewegingssensor \& Automatische Lux-dimming](#6-display-pir-bewegingssensor--automatische-lux-dimming)
    - [7. Telemetrie, MQTT, Syslog \& Telegram-interactie](#7-telemetrie-mqtt-syslog--telegram-interactie)
      - [Grafana Dashboard Tijdlijn](#grafana-dashboard-tijdlijn)
      - [Telegram commando's](#telegram-commandos)

## 🇳🇱 Nederlands

### Firmware Versie`v3.3.0`

**Target Hardware:** ESP32-S3-ZERO-N8R8-M

### Netwerk & OTA Stabiliteit (De Grote Doorbraak)

- **AP/OTA Loops opgelost**: De netwerk initialisatie (ArduinoOTA.begin() binnen setupNetwork()) gecentraliseerd om hardnekkige opstartloops naar Access Point-modus en post-flash bevriezingen definitief te elimineren.

- **Directe OTA Reboot**: Het systeem herstart nu direct en vlekkeloos zodra een software-update succesvol is voltooid.

### Telegram Bot & Commando Suite (v3.3.0)

- **Commando-routing hersteld**: De bericht-matching in `Telegram.cpp` opgeschoond, waardoor commando's (inclusief `/h` en `/help`) weer direct reageren zonder onterechte "Onbekend commando" meldingen.

- **Fan-commando's gestroomlijnd en hernoemd**:

  - `/tfs` — Test Sweep (5 min ramp MIN ↔ MAX)
  - `/tfl` — Alle 3 de fans 30s op `MIN_PWM`
  - `/tfh` — Alle 3 de fans 30s op `MAX_PWM`
  - `/tfv` (Test Van Vent) — Interne fan UIT, externe fans max PWM (30s)
  - Overbodige handmatige overrides (`/mf`) zijn opgeruimd.

- **Nieuwe klimaat-test commando's toegevoegd**:

  - `/tu` (Test Uitdroging) — Simuleert hoge VPD / te droge kas.
  - `/tk` (Test Klam) — Simuleert klamme kas / schimmelrisico.
  - `/tw` (Test Woning) — Simuleert de woning-ventilatiebuffer.

- **Menu-opmaak gecorrigeerd**: Alle Markdown-foutjes en afbrekingen in het help menu rechtgezet zodat Telegram het weer strak en uniform uiterlijk geeft.

### Systeem & Versiebeheer

- **Versie en Build Timestamps**: Firmware-versie definitief vastgezet op v3.3.0, gekoppeld aan automatische build timestamps (`__DATE__` `__TIME__`) die netjes via netwerk info logs en opstartmeldingen (`/ni`) worden uitgelezen.

## Handleiding & Documentatie

📖 **Handleiding: Robuuste Klimaatregeling, Teeltlogica & Telemetrie-architectuur**

Deze handleiding beschrijft de technische en teelttechnische opzet van het ESP32-S3 kiemgroente-kas systeem ("De kleine kas"). Het is ontworpen om volautomatisch, 'headless' en extreem bedrijfszeker te draaien in huis.

### 1. Hardware-architectuur, Pinout & Systeemopzet

Het hart van de installatie is een **ESP32-S3-ZERO-N8R8-M** (gedraaid op 160 MHz voor optimale warmte- en energiehuishouding), die I2C-sensoren, PWM-aansturingen en interrupts beheert.

- **Kas Sensoren (SHT31)**: Industriële I2C-sensor voor hoge precisie temperatuur-, luchtvochtigheid-, VPD- en dauwpuntmetingen. Uitgerust met een softwarematig aanstuurbare interne heater voor herstel en kalibratie.
- **Woning Sensoren (BME280)**: Meet referentiewaarden voor temperatuur, luchtvochtigheid én luchtdruk in de woning (bufferzone).
- **Licht & Beweging**: BH1750 lichtsensor (lux) en een PIR-bewegingssensor (`PIN_PIR` op GPIO 2) voor automatische display-inschakeling en kweeklamp-detectie.
- **PWM Fans (4-Wire, 3 stuks)**:
- `PIN_FAN_INT_PWM` (GPIO 12) & `PIN_FAN_INT_TACHO` (GPIO 13): Interne circulatieventilator.
- `PIN_FAN_EXT1_PWM` (GPIO 10) & `PIN_FAN_EXT1_TACHO` (GPIO 11): Externe afzuig-/toevoerventilator 1.
- `PIN_FAN_EXT2_PWM` / TACHO: Externe afzuig-/toevoerventilator 2.
- Aangestuurd op **25 kHz PWM** om motorgezoem volledig te elimineren. Tacho-lijnen zijn beveiligd via diodes en worden uitgelezen via hardware-interrupts (`FALLING`).

- **Actuators & Status**: Warmtemat aansturingslogica en onboard Status Neopixel LED (`PIN_NEOPIXEL` op GPIO 21).

### 2. Teeltlogica Kiemgroenten: Waarom VPD, Drempels & Buffers Cruciaal Zijn

Het opkweken van microgreens (kiemgroenten) in een afgesloten ruimte luistert uiterst nauw:

- **VPD (Vapor Pressure Deficit - Dampdrukverschil)**: De graadmeter voor gewasverdamping.
- **Te laag (`< 0.60 kPa`)**: Stilstaande, klamme lucht waarin schimmels (*Botrytis*, kiemrot) binnen enkele uren toeslaan.
- **Te hoog (`> 0.80 kPa`)**: Veroorzaakt snelle uitdroging van de teer gewortelde kiemplanten.

- **Dauwpuntmarge (`DP_MARGIN_MIN = 2.0°C`)**: Voorkomt dat het bladblad doordringt tot het dauwpunt, wat schadelijke condensvorming op het gewas voorkomt.
- **De Woning als tussen buffer**: Buitenlucht is vaak te koud of vochtig. Door de **woonkamer** als buffer te gebruiken, ventileert de kas met stabiele en drogere binnenlucht zonder thermische shocks.

### 3. Kerncomponenten van het Klimaatregelsysteem (`ClimateLogic.cpp`)

1. **Sensor Filter (Voortschrijdend Gemiddelde)**: Filtert metingen via een buffer (`SENSOR_BUFFER_SIZE = 3`) om meetruis en zenuwachtig schakelen (*chattering*) van ventilatoren te voorkomen.
2. **Anti-Hunt Beveiliging**: Dwingt een minimale looptijd af (`MIN_FAN_RUN_TIME = 180000 ms / 3 minuten`) bij statusveranderingen van ventilatoren.
3. **Proactieve Snelheidsbewaking (Velocity Check)**: Detecteert een snelle stijging van de luchtvochtigheid (`humidityVelocity`). Een stijging van $\ge 3\%$/minuut boven de 60% LV activeert direct interne circulatie *voordat* schimmelrisico ontstaat.
4. **Boot-Bypass & Lampdetectie**: Als de lichtsensor bij het opstarten meer dan 1.0 lux meet (kweeklampen aan), wordt de standaard kalibratiewachttijd van 3 minuten overgeslagen en start de klimaatregeling direct.
5. **NVS Geheugenopslag**: Onthoudt de slaapstand-status (`isKasSleeping`) na reboots of stroomuitval via de `Preferences` van de ESP32-S3.

### 4. Gelaagde Fan-besturing & Klimaatbeveiliging

- **Groene Zone (Optimaal)**: VPD tussen `0.60` en `0.80` kPa. Klimaat is stabiel; fans staan in rust.
- **Oranje Zone (Soft Waarschuwing)**: Lichte klamheid. Interne fan draait rustig (~45% PWM) om het microklimaat rond de kiemen te breken.
- **Rode Zone (Hard / Kritiek)**: Grotere VPD-afwijking of condensrisico. Externe fans schalen dynamisch op tot 100%.
- **Ultra Max (Noodtoestand / Klimaatcrash)**: Overschrijdt de kas-temperatuur de kritieke grens (`GREENHOUSE_CRASH_TEMP`, bijv. 28°C / 30°C), dan schakelen alle externe fans naar 100% en wordt direct een Telegram-noodalarm verzonden.
- **Slaapstand (`/sl`)**: Schakelt actieve ventilatie uit (PWM naar 0%), terwijl sensoren op de achtergrond data blijven loggen.

### 5. Slimme Omgevingsbeoordeling & 3-Klimaten Keten

Vergelijkt de toestand in de Kas, Woning en Buiten (via OpenWeather API) om te bepalen of ventilatie met woning- of buitenlucht veilig en effectief is.

### 6. Display, PIR-bewegingssensor & Automatische Lux-dimming

- **PIR-sturing**: Het OLED-scherm staat standaard uit en springt aan bij beweging (30 seconden timeout).
- **Lux-dimming**: Dimt het display automatisch naar stand `10` wanneer het omgevingslicht onder `1.0` lux zakt.

### 7. Telemetrie, MQTT, Syslog & Telegram-interactie

Draadloze integratie met HGrafana Dashboard via MQTT on Raspberry Pi server (inclusief lokale LittleFS-buffering bij netwerkuitval), Syslog logging, Telnet-monitoring, OTA updates en Telegram-commando's:

#### Grafana Dashboard Tijdlijn

![Grafana Dashboard 1](/src/img/Grafana%20Dashboard%201.png)

![Grafana Dashboard 2](/src/img/Grafana%20Dashboard%202.png)

![Grafana Dashboard 3](/src/img/Grafana%20Dashboard%203.png)

#### Telegram commando's

- **Informatie & Rapporten:**
- `/st` of `status` — Hoofdrapport met actuele VPD, temp, LV en kas acties.
- `/hc` or `hwcheck` — Hardwarecheck (RAM, chip-temp, sensor fout-percentages, lux, PIR, Fan RPMs).
- `/mm` of `minmax` — Min/max waarden van de dag per locatie.
- `/ad` of `advies` — Uitgebreid teelt-, ventilatie- en schimmeladvies.
- `/ni` of `netwerkinfo` — Netwerkstatus, RSSI, IP, firmware versie (`v3.2.9`) en build-tijdstempel.
- `/h`, `help` of `start` — Toont het commando-menu.

- **Testen & Overrides:**
- `/tf` — Test alle fans gedurende 30 sec op 100% PWM.
- `/tf2` — Interne fan UIT, externe fans 1 & 2 op 100% PWM (30 sec).
- `/mf` — Handmatige TFM-override (interne fan UIT, externe fans 100%).
- `/tm` — Test warmtemat status (15 sec test).
- `/ts` — Test schimmelrisico status (10 min test).
- `/tc` — Simuleert 72°C chiptemperatuur alarm.
- `/fa` — Schakelt fan-alarmeringen via Telegram in of uit.

- **Systeem & Onderhoud:**
- `/sl` of `slaapstand` — Schakelt slaapstand in/uit (status opgeslagen in NVS).
- `/cal` of `kalibratie` — Zet alle fans stil en triggert een SHT31 heater-check (indien lampen uit zijn).
- `/ota` — Start lokaal AP voor draadloze OTA firmware-updates.
- `/fl` of `flush` — Maakt Telegram-wachtrij leeg.
- `/rb` of `reboot` — Herstart de ESP32-S3 direct.
- `/pe` of `paasei` — *[Geheime Easter Egg]* Roterende kiemgroenten-feitjes en humor.

![Telegram 1](/src/img/Telegram%201.png)

![Telegram 2](/src/img/Telegram%202.png)

![Telegram 3](/src/img/Telegram%203.png)
