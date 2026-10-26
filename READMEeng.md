# ESP32-S3 Microgreens Climate Control System

- [ESP32-S3 Microgreens Climate Control System](#esp32-s3-microgreens-climate-control-system)
  - [🇬🇧 English](#-english)
    - [About the Project](#about-the-project)
    - [What's New? v3.5.4](#whats-new-v354)
    - [Key Features in v3.5.3](#key-features-in-v353)
    - [🇬🇧 v3.5.2](#-v352)
    - [v3.5.1](#v351)
    - [Version v3.5 is officially live!](#version-v35-is-officially-live)
    - [Upcoming v3.5](#upcoming-v35)
    - [v3.4.4](#v344)
    - [What's New in De kleine kas v3.4.3](#whats-new-in-de-kleine-kas-v343)
    - [🌀 Fan Architecture \& Test Modes (Optimized)](#-fan-architecture--test-modes-optimized)
    - [📊 Tacho \& RPM Calibration](#-tacho--rpm-calibration)
    - [🛠️ System \& Observability](#️-system--observability)
    - [Manual \& Documentation](#manual--documentation)
      - [1. Hardware Architecture, Pinout \& System Setup](#1-hardware-architecture-pinout--system-setup)
      - [2. Microgreens Agronomy: Why VPD, Thresholds \& Buffers Matter](#2-microgreens-agronomy-why-vpd-thresholds--buffers-matter)
      - [3. Core Components of the Climate Control System (`ClimateLogic.cpp`)](#3-core-components-of-the-climate-control-system-climatelogiccpp)
      - [4. Layered Fan Control \& Climate Safety](#4-layered-fan-control--climate-safety)
      - [5. Environmental Assessment \& 3-Climate Chain](#5-environmental-assessment--3-climate-chain)
      - [6. Display, PIR Motion Sensor \& Automatic Lux Dimming](#6-display-pir-motion-sensor--automatic-lux-dimming)
      - [7. Telemetry, MQTT, Syslog \& Telegram Commands](#7-telemetry-mqtt-syslog--telegram-commands)
      - [Grafana Dashboard Timeline](#grafana-dashboard-timeline)
      - [Telegram commands](#telegram-commands)

## 🇬🇧 English

### About the Project

**De Kleine Kas** is an advanced climate and greenhouse controller built on an **ESP32-S3-Zero (N8R8)**. The firmware autonomously regulates the indoor climate using **VPD (Vapor Pressure Deficit)**, manages dual tray heating with independent germination and growth profiles, and enforces safety through a unique **3-zone architecture (Greenhouse $\iff$ Indoor Living Space $\iff$ Outdoor)**.

### What's New? v3.5.4

- **Dynamic Feedforward Lamp Anticipation (The 21-Hour Rule):** The system now dynamically tracks when greenhouse lights turn on and initiates proactive indoor buffer conditioning 3 hours prior (within a 24-hour cycle) to optimize moisture buffering (`targetIndoorVpdMin = 0.95 kPa`).
- **Code Architecture & Scope Fixes:** `upcomingLightsOn` and timing variables have been structured using file-scope variables alongside a clean getter function (`getMinutesUntilLightsOn()`), resolving all scope-related compilation errors.
- **Telegram `/st` Status Report Integration:** Requesting `/st` on-demand now seamlessly displays the current anticipation status and an exact countdown (hours and minutes) until the lights activate.
- **Silent Background Operation:** Zero push notification spam for routine adjustments; push alerts remain strictly reserved for critical alarms and unresolved climate escalations, while on-demand transparency via `/st` remains fully detailed.

### Key Features in v3.5.3

- **3-Zone Climate & Buffer Hierarchy:** The greenhouse exchanges air exclusively with the indoor living space. The indoor space acts as a thermal buffer and is continuously evaluated against outdoor weather via dew point calculations (`houseAdvice`).
- **Independent Tray Monitoring:** Two DS18B20 sensors track individual trays with selectable modes (Germination, Growth, Heat Mat) backed by persistent NVS storage.
- **Anti-Oscillation & Self-Learning:** *Inertia Learning* and dynamic setpoint offsets suppress hunting and toggling behavior under changing thermal loads.
- **Fail-Safe PWM Limits:** No dead zones between 1 and `MIN_PWM`. Fans operate strictly at 0 or `>= MIN_PWM` with a hard-coded startup kick (`BASE_PWM`).
- **Offline LittleFS Buffering:** Telemetry payloads are safely cached locally in LittleFS during network outages and non-blockingly flushed upon reconnection.
- **Fixed OLED Display Layout:** Greenhouse (left) and Indoor (right) remain locked in position; the top banner smoothly alternates between outdoor weather and tray temperatures.

---

### 🇬🇧 v3.5.2

1. **Complete Configuration Centralization**: All remaining magic numbers, thresholds, and safety margins (such as VPD error margins, crash offsets, and humidity velocity triggers) have been moved from the logic files into Config.h for centralized management.
2. **Smooth PID Fan Control**: By lowering the proportional gain (kp to 0.5) and externalizing the VPD_MAX_ERROR_THRESHOLD, the harsh "bang-bang" (on/off) fan behavior has been replaced with smooth, proportional PWM modulation.
3. **Real-Time Grafana & Data Flow**: Data flow has been synchronized, and Grafana dashboard queries have been updated to fixed time windows (time >= now() - 1h) using fill(previous) and rapid grouping (GROUP BY time(10s)), permanently eliminating the previous one-hour reaction delay.
4. **Code Refactoring**: Config.h has been completely reorganized into logical, well-documented sections for easier maintenance.

### v3.5.1

1. TraySensors min/max added
2. Outdoor min/max N/A fix for relative humidity
3. Telegram updated

### Version v3.5 is officially live!

With this update, a robust and intelligent architecture is running on the ESP32-S3:

- **Three NVS-persistent Tray Modes**: `ts1k/ts2k`, `ts1g/ts2g`, and `ts1wm/ts2wm` for Germination, Growth, and Heating Mat modes.
- **Germination Stage Resting State**: Greenhouse active ventilation and light management are bypassed when all trays are set to Germination mode, while tray safety sensors and room/outdoor buffers remain actively monitored.
- **Dynamic PID-Driven Fan Control**: Fans smoothly scale in real time with the PID controller output rather than relying on static step thresholds.
- **System Load Status Icons**: Instantly check status in Telegram to see if the greenhouse is resting (⚪), initializing/calibrating (🟡), actively controlling climate (🔵), operating under high system load (🟠), or requiring manual intervention (🔴).
- **Smart Cultivation Advisory (`/kt`)**: Delivers insights into optimal lighting windows based on indoor buffer conditions.

### Upcoming v3.5

- Advise what times are better to enable/disable De kleine kas, climate and energy costs wise.

### v3.4.4

Added:

- 2 BS18D20 temp sensors as tray sensors
- Germination vs grow fase
- Checks & warnings for both modes
- Decisions for both modes

### What's New in De kleine kas v3.4.3

### 🌀 Fan Architecture & Test Modes (Optimized)

- **Pure Direct Pin Writes:** Completely stripped complex ramping and artificial limiting logic; pins are now driven directly and instantaneously via `ledcWrite` on the ESP32-S3 (Core v3.x).
- **Smart Toggle Test Modes:** Test commands (`/tfl`, `/tfh`, `/tfs`, `/tfv`) now function as a direct on/off toggle. Automatic 30-second timeouts are gone; simply send the same command again to instantly stop the test and resume automatic climate regulation.
- **Stepped Fan Sweep (`/tfs`):** The sweep test is structured into fixed 30-second blocks per level (Min $\rightarrow$ Mid $\rightarrow$ Max $\rightarrow$ Mid). This perfectly aligns with your Grafana telemetry interval, resulting in a clean, readable step-wave pattern in your graphs.

### 📊 Tacho & RPM Calibration

- **Clean External Fan Mapping:** Electrical 'chopping' noise at low PWM voltages (such as 25% / PWM 65), which previously caused tacho readings to stick at a false 100 RPM, has been resolved. External fans now map cleanly and linearly between the hardware specs of **300 RPM (at PWM 65)** and **1500 RPM (at PWM 255)**.
- **Internal Fan Isolation:** The internal circulation fan is fully decoupled from external fan tests, retaining its own reliable pulse counting and baseline behavior.

### 🛠️ System & Observability

- **MQTT & Grafana Synchronization:** All manual overrides, test flags, and calibrated RPM values are immediately broadcasted to the broker for real-time observability in Grafana.
- **Ready for Expansion:** Prepared for the integration of the two additional DS18B20 sensors for substrate and tray temperature monitoring.

---

### Manual & Documentation

📖 **Manual: Robust Climate Control, Agronomic Logic & Telemetry Architecture**

This manual describes the technical and agronomic architecture of the ESP32-S3 microgreens greenhouse system ("De kleine kas"). It is engineered to run completely headless, combining strict agronomic safety margins with robust industrial patterns.

#### 1. Hardware Architecture, Pinout & System Setup

The brain of the system is an **ESP32-S3-ZERO-N8R8-M** (running at 160 MHz for optimal thermal and power efficiency), communicating via I2C, PWM, and Interrupts.

- **Greenhouse Sensors (SHT31)**: Industrial high-precision I2C sensor measuring temperature, humidity, VPD, and dew point. Features a built-in software-controllable internal heater for sensor recovery and calibration.
- **Indoor Sensors (BME280)**: Measures reference indoor temperature, humidity, and barometric pressure from the living space buffer zone.
- **Light & Motion**: BH1750 ambient light sensor (lux) and a PIR motion sensor (`PIN_PIR` on GPIO 2) for automatic OLED display wakeup and grow light detection.
- **PWM Fans (4-Wire, 3 Units)**:
- `PIN_FAN_INT_PWM` (GPIO 12) & `PIN_FAN_INT_TACHO` (GPIO 13): Internal circulation fan.
- `PIN_FAN_EXT1_PWM` (GPIO 10) & `PIN_FAN_EXT1_TACHO` (GPIO 11): External intake/exhaust fan 1.
- `PIN_FAN_EXT2_PWM` / TACHO: External intake/exhaust fan 2.
- Operating at **25 kHz PWM** to eliminate audible motor hum. Tachometer lines are protected via inline diodes and read via hardware interrupts (`FALLING`).

- **Actuators & Status**: PWM Heat mat recommendation logic and onboard Status Neopixel LED (`PIN_NEOPIXEL` on GPIO 21).

#### 2. Microgreens Agronomy: Why VPD, Thresholds & Buffers Matter

Microgreens are extremely sensitive to microclimates in enclosed spaces:

- **VPD (Vapor Pressure Deficit)**: Primary metric for transpiration.
- **Too Low (`< 0.60 kPa`)**: Stagnant, clammy air leading to fungal outbreaks (*Botrytis*, damping-off) within hours.
- **Too High (`> 0.80 kPa`)**: Causes severe root dehydration and stalls growth.

- **Dew Point Margin (`DP_MARGIN_MIN = 2.0°C`)**: Prevents leaf surface temperature from approaching the dew point, avoiding destructive leaf condensation.
- **Indoor Buffer Strategy**: Instead of drawing cold/damp outdoor air directly into a small greenhouse, the system leverages conditioned indoor air from the living room as a stable climate buffer.

#### 3. Core Components of the Climate Control System (`ClimateLogic.cpp`)

1. **Moving Average Sensor Filter**: Maintains a sliding buffer (`SENSOR_BUFFER_SIZE = 3`) to eliminate sensor noise and prevent actuator chattering.
2. **Anti-Hunt Protection**: Enforces a minimum fan run time (`MIN_FAN_RUN_TIME = 180000 ms / 3 min`) to protect motor bearings and electronics.
3. **Proactive Humidity Velocity Check**: Monitors rate of humidity rise (`humidityVelocity`). Surges $\ge 3\%$/min above 60% RH trigger instant internal circulation *before* mold risk establishes.
4. **Boot-Bypass & Light Detection**: If ambient light exceeds 1.0 lux during boot (grow lights detected), the standard 3-minute calibration delay is bypassed, immediately activating automated climate control.
5. **NVS State Memory**: Retains sleep mode (`isKasSleeping`) across reboots via ESP32 `Preferences` non-volatile storage.

#### 4. Layered Fan Control & Climate Safety

- **Green Zone (Optimal)**: VPD between `0.60` and `0.80` kPa. Climate stable; fans idle.
- **Orange Zone (Soft Warning)**: Minor clamminess. Internal fan circulates softly (~45% PWM) to break the boundary layer.
- **Red Zone (Hard / Critical)**: Significant VPD deviation or condensation risk. External fans scale dynamically up to 100% capacity.
- **Ultra Max (Emergency Crash)**: Triggered if greenhouse temperature exceeds `GREENHOUSE_CRASH_TEMP` (28°C / 30°C). All external fans force 100% duty cycle with an instant Telegram emergency alert.
- **Sleep Mode (`/sl`)**: Climate regulation pauses and fan PWM signals drop to 0%, while sensors continue logging background data.

#### 5. Environmental Assessment & 3-Climate Chain

Compares Greenhouse vs. Indoor vs. Outdoor (via OpenWeather API) to dynamically decide whether to vent, circulate, or seal the greenhouse enclosure.

#### 6. Display, PIR Motion Sensor & Automatic Lux Dimming

- **PIR Display Control**: OLED display sleeps by default and wakes for 30 seconds upon motion detection.
- **Lux Dimming**: Automatically dims display brightness to `10` when ambient light drops below `1.0` lux at night.

#### 7. Telemetry, MQTT, Syslog & Telegram Commands

Integrates with Grafana Dashboard via MQTT on Raspberry Pi server (with local LittleFS buffering for offline recovery), Syslog logging, Telnet monitoring, Wireless AP OTA updates, and Telegram bot commands:

#### Grafana Dashboard Timeline

![Grafana Dashboard 1](/src/img/Grafana%20Dashboard%201.png)

![Grafana Dashboard 2](/src/img/Grafana%20Dashboard%202.png)

![Grafana Dashboard 3](/src/img/Grafana%20Dashboard%203.png)

#### Telegram commands

- **Status & Reports:**
  - `/st` or `status` — Main report (VPD, Temp, RH per location, and active logic).
  - `/hc` or `hwcheck` — Detailed hardware check (Free RAM, Chip Temp, sensor error rates, lux, PIR, Fan RPMs).
  - `/mm` or `minmax` — Daily min/max temperature & humidity per location.
  - `/ad` or `advies` — Detailed cultivation, ventilation, and mold diagnostics.
  - `/ni` or `netwerkinfo` — Network status, RSSI, IP, firmware version (`v3.2.9`), and build timestamp.
  - `/h`, `help`, or `start` — Displays the command menu.

- **Testing & Overrides:**
  - `/tf` — Tests all fans for 30 sec at 100% PWM.
  - `/tf2` — Internal fan OFF, external fans 1 & 2 at 100% PWM (30 sec).
  - `/mf` — Manual TFM override toggle (internal fan OFF, external fans 100%).
  - `/tm` — Manually toggles heat mat state (15 sec test).
  - `/ts` — Manually toggles mold risk state (10 min test).
  - `/tc` — Simulates 72°C chip temperature security alert.
  - `/fa` — Enables or disables (mutes) fan alerts via Telegram.

- **System & Maintenance:**
  - `/sl` or `slaapstand` — Toggles sleep mode (state saved to NVS).
  - `/cal` or `kalibratie` — Stops all fans and runs an SHT31 sensor heater test (if lights are off).
  - `/ota` — Starts local AP mode for wireless OTA firmware updates.
  - `/fl` or `flush` — Clears incoming Telegram queue.
  - `/rb` or `reboot` — Restarts the ESP32-S3 immediately.
  - `/pe` or `paasei` — *[Secret Easter Egg]* Microgreens facts and jokes.

![Telegram 1](/src/img/Telegram%201.png)

![Telegram 2](/src/img/Telegram%202.png)

![Telegram 3](/src/img/Telegram%203.png)
