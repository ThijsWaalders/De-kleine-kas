# ESP32-S3 Kiemgroente Klimaat Regeling

- [ESP32-S3 Kiemgroente Klimaat Regeling](#esp32-s3-kiemgroente-klimaat-regeling)
  - [Over het Project](#over-het-project)
  - [Wat is er nieuw?](#wat-is-er-nieuw)
    - [v3.5.5](#v355)
    - [v3.5.4](#v354)
    - [v3.5.3](#v353)
    - [v3.5.2](#v352)
    - [v3.5.1](#v351)
    - [v3.5](#v35)
    - [v3.4.4](#v344)
    - [v3.4.3](#v343)
      - [🌀 Fan-architectuur \& Testmodi (Geoptimaliseerd)](#-fan-architectuur--testmodi-geoptimaliseerd)
      - [📊 Tacho \& RPM Kalibratie](#-tacho--rpm-kalibratie)
      - [🛠️ Systeem \& Observability](#️-systeem--observability)
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

## Over het Project

**De Kleine Kas** is een geavanceerd, robuust klimaat- en kweekstation voor een indoor kweekkas, aangestuurd door een **ESP32-S3-Zero (N8R8)**. De firmware regelt autonoom het binnenklimaat op basis van **VPD (Vapor Pressure Deficit)**, beheert tweekanaals tray-verwarming met onafhankelijke kiem- en groeimodus, en bewaakt de veiligheid via een unieke **3-zone hiërarchie (Kas $\iff$ Woning $\iff$ Buiten)**.

Hier is het overzicht van wat er recent is toegevoegd en geoptimaliseerd:

## Wat is er nieuw?

### v3.5.5

- **Centrale & Lamp-Aware VPD Grenzen**: Alle VPD-drempels zijn gecentraliseerd in Config.h en zijn nu volledig "lamp-aware". Het systeem onderscheidt automatisch strakke dagwaarden van ruimere nachtwaarden (VPD_MIN/MAX_OPTIMAL_DAY/NIGHT).

- **Slimme Nachtdip (Geen Valse Alarmen)**: De vochtigheidsgrens voor actieve correctie (HUM_MOLD_THRESHOLD) is verruimd naar 80%. Hierdoor triggert de natuurlijke nachtelijke dip geen onnodige stressmeldingen of klam-alarmen meer.

- **Grafana-Geoptimaliseerde Statusredenen**: Alle statusredenen (kasAdviceReason) in de beslisboom zijn opgeschoond. Overbodige emoji's, markdown-sterretjes en lange, afgebroken zinnen zijn verwijderd. Dit zorgt voor een superstrakke, overzichtelijke en professionele tijdlijnweergave in Grafana.

- **Volledige Technische Harmonisatie**: Modules zoals ClimateLogic, DecisionEngine, Display, MQTT en Telegram grijpen nu allemaal foutloos en zonder losse "magic numbers" terug op dezelfde centrale configuratiebron.

### v3.5.4

- **Dynamische Feedforward Lamp-anticipatie (De 21-uur regel):** Het systeem leert nu automatisch wanneer de verlichting aanspringt en start precies 3 uur vóór dat verwachte moment (binnen een 24-uurs cyclus) met het proactief conditioneren van de woningbuffer (`targetIndoorVpdMin = 0.95 kPa`).
- **Code-architectuur & Scope Fixes:** `upcomingLightsOn` en de timingvariabelen zijn omgezet naar file-scope variabelen met een schone getter (`getMinutesUntilLightsOn()`), waardoor alle compiler-fouten zijn opgelost.
- **Telegram `/st` Statusrapport Integratie:** Wanneer je handmatig `/st` opvraagt, toont het rapport nu direct de actieve anticipatiestatus en een nauwkeurige countdown (in uren en minuten) tot de lampen weer aanspringen.
- **Stilte op de Achtergrond:** Geen onnodige pushmeldingen voor routine-aanpassingen; push-alerts zijn strikt voorbehouden aan kritieke alarmen en mislukte klimaat-escalaties. Volledige transparantie blijft wel beschikbaar op on-demand basis via `/st`.

### v3.5.3

- **3-Zone Klimaat- en Bufferlogica:** De kas wisselt gecontroleerd lucht uit met de woning. De woning fungeert als thermische buffer en wordt getoetst tegen de buitenlucht via dauwpuntsberekening (`houseAdvice`).
- **Onafhankelijke Tray-Bewaking:** Twee DS18B20 sensoren bewaken de trays afzonderlijk met instelbare modi (Kiemfase, Groeifase, Warmtemat) en persistente opslag via Preferences (NVS).
- **Slimme Anti-Jojo & Zelflerende Regeling:** *Inertia Learning* en dynamische setpoint-offsets voorkomen oscillerend gedrag bij wisselende warmtelasten.
- **Waterdichte PWM-veiligheid:** Geen "niemandsland" tussen 1 en `MIN_PWM`. Ventilatoren draaien óf op 0, óf op minimaal `MIN_PWM` met een harde `BASE_PWM` kickstart.
- **Offline LittleFS Buffering:** MQTT-metingen worden lokaal opgeslagen in LittleFS bij netwerkuitval en non-blocking ingehaald zodra de verbinding herstelt.
- **Vast OLED Display Layout:** Kas (links) en Woning (rechts) staan op een vaste locatie; de bovenste regel roteert slim tussen buitenweer en tray-temperaturen.

---

### v3.5.2

1. **Volledige Configuratie-centralisatie**: Alle resterende 'magic numbers', harde drempelwaarden en veiligheidsmarges (zoals de VPD-foutmarge, crash-offsets en vochtigheidssnelheid-triggers) zijn uit de logica gehaald en ondergebracht in Config.h. Dit maakt het beheer een stuk overzichtelijker.
2. **Vloeiende PID-ventilatorregeling**: Door de proportionele factor (kp) te verlagen naar 0.5 en de VPD_MAX_ERROR_THRESHOLD configureerbaar te maken, is de abrupte "bang-bang" (aan/uit) aansturing van de ventilatoren veranderd in een soepele, proportionele PWM-modulatie.
3. **Real-Time Grafana & Data Flow**: De datastroom is geoptimaliseerd en de Grafana-dashboardqueries zijn bijgewerkt naar een vast tijdsvenster (time >= now() - 1h) met fill(previous) en snelle groepering (GROUP BY time(10s)). Hierdoor is de oude vertraging van een uur definitief verdwenen.
4. **Code Schoonmaak**: Config.h is volledig heringedeeld in logische, gedocumenteerde secties voor optimaal onderhoud.

### v3.5.1

1. TraySensors min/max toegevoegd
2. Outdoor min/max N/A fix voor relatieve luchtvochtigheid
3. Telegram bijgewerkt

### v3.5

Met deze update staat er echt een ijzersterke en slimme architectuur op de ESP32-S3:

1. **Drie NVS-persistent opgeslagen tray-modi**: `ts1k/ts2k`, `ts1g/ts2g`, `ts1wm/ts2wm` voor kiem, groei en warmtemat.
2. **Ruststand in de kiemfase**: De kasventilatie en lampsturing gaan uit als alle trays op kiemmodus staan, terwijl de tray-sensoren en buffers actief bewaakt blijven.
3. **Actief meeademende PID-ventilatiesturing**: De fans ademen nu vloeiend mee met de PID-output in plaats van op een statische lijn te hangen.
4. **Belastingsindicatie met icoontjes**: In één oogopslag in Telegram zien of de kas in rust is (⚪), opstart (🟡), strak bijstuurt (🔵), zwoegt op grensbelasting (🟠) of dat jij actie moet ondernemen (🔴).

Slimme Kweektijden (`/kt`): Inzicht in de meest optimale belichtingstijden op basis van de woningbuffer.

### v3.4.4

Toegevoegd:

- 2 BS18D20 temp sensors als tray sensors
- Kiem vs groei fase modus per sensor
- Checks & waarschuwingen voor beide modus
- Beslisboom aangepast voor beide modus

### v3.4.3

#### 🌀 Fan-architectuur & Testmodi (Geoptimaliseerd)

- **Pure Direct Pin Writes:** Volledig afgestapt van kunstmatige ramping of vertragende limits; pinnen worden direct en bliksemsnel aangestuurd via `ledcWrite` op de ESP32-S3 (Core v3.x).
- **Slimme Toggle-testmodi:** Testcommando's (`/tfl`, `/tfh`, `/tfs`, `/tfv`) werken nu als een directe aan/uit-toggle. Geen automatische timeouts van 30 seconden meer; stuur het commando nogmaals om de test per direct te beëindigen en de automatische regeling te hervatten.
- **Trapvormige Fan Sweep (`/tfs`):** De sweep-test is opgedeeld in vaste stappen van 30 seconden per niveau (Min $\rightarrow$ Midden $\rightarrow$ Max $\rightarrow$ Midden). Hierdoor sluit de cyclus naadloos aan op het telemetrie-intervallen van Grafana en zie je een strakke, heldere golfbeweging in je grafieken.

#### 📊 Tacho & RPM Kalibratie

- **Zuivere Externe Fan Mapping:** Elektrische 'chopping'-ruis op lage PWM-spanningen (zoals 25% / PWM 65) die tacho-metingen deed hangen op valse 100 RPM is opgelost. De externe fans mappen nu zuiver en lineair tussen de hardware-specificatie van **300 RPM (bij PWM 65)** en **1500 RPM (bij PWM 255)**.
- **Interne Fan Isolatie:** De interne circulatieventilator is volledig ontkoppeld van de externe ventilatortests en behoudt zijn eigen betrouwbare pulstelling en basisgedrag.

#### 🛠️ Systeem & Observability

- **MQTT & Grafana Synchronisatie:** Alle handmatige overrules, testvlaggen en gekalibreerde toerentalwaarden worden direct doorgegeven aan de broker voor real-time inzicht in Grafana.
- **Klaar voor uitbreiding:** Voorbereid op de integratie van de twee extra DS18B20-sensoren voor substraat- en traytemperatuurmetingen.

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
