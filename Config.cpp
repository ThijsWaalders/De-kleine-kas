/**
 * @file Config.cpp
 * @brief Implementatie van globale variabelen en objecten voor de kascontroller (ESP32-S3).
 */

#include "nono.h" 
#include "Config.h"

// =========================================================================
// 1. PID REGELAAR VARIABELEN (Gekoppeld aan de instellingen in Config.h)
// =========================================================================
float kp = 1.5;                     // Proportioneel (directe reactie)
float ki = 0.15;                    // Integraal (opbouw bij langdurig vocht)
float kd = 0.6;                     // Derivatief (remming op schommeling)


// =========================================================================
// 2. SYSTEEM & HARDWARE STATEN
// =========================================================================
const float ESP_CRITICAL_TEMP = 70.0;

bool mfOverrideActive = false;
bool isKasSleeping = false;
bool pendingTelegramAlert = false;

// Interne temperatuursensor handle (veilig verborgen in Config.cpp)
static temperature_sensor_handle_t global_temp_sensor_handle = NULL;
static bool tempSensorInitialized = false;

// PIR sensor Display de-/activatie
bool isDisplayActiveByMotion = true;
unsigned long lastMotionTime = 0;


// =========================================================================
// 3. NETWERK & SERVERS (Gekoppeld aan nono.h)
// =========================================================================
char ssid [] = SECRET_SSID;
char password [] = SECRET_PASS;
String openWeatherKey = API_KEY;
String cityID = CITY_ID;

char telbot[] = BOT_TOKEN;
char telid[] = MY_CHAT_ID;

unsigned long lastSuccessfulNetworkActivity = 0;
bool isConnected = false;
float espVcc = 0.0;
int wifiRSSI = 0;
uint32_t freeHeap = 0;
const char* BUFFER_FILE = "/data_buffer.txt";


// =========================================================================
// 4. SENSOR & KLIMAAT VARIABELEN
// =========================================================================
unsigned long totalKasDhtReads = 0;
unsigned long failedKasDhtReads = 0;
unsigned long lastValidKasDhtTime = 0;
unsigned long totalIndoorDhtReads = 0;
unsigned long failedIndoorDhtReads = 0;
unsigned long lastValidIndoorDhtTime = 0;

float kasSmoothedTemp = -999.0, kasSmoothedHum = -999.0, indoorSmoothedTemp = -999.0, indoorSmoothedHum = -999.0, displayedTemp = -999.0;
unsigned long lastTempUpdate = 0;

float outdoorTemp = 0.0, outdoorTempMax = 0.0, outdoorTempMin = 0.0, outdoorHumidity = 0.0, outdoorDewPoint = 0.0, outdoorPressure = 0.0;
bool outdoorMoldRisk = false, hasOutdoorAlert = false, hasWeatherAlarm = false;
String weatherDesc = "", outdoorPressureTrendText = "=";
int weatherID = 0;
unsigned long lastWeatherUpdate = 0;

float forecastedTempSoon = 0.0;
bool isTempDroppingSoon = false;

bool isSpaceHeatingNeeded = false;
bool isHeatMatRecommended = false;

bool isTestModeActive = false;
unsigned long testModeStartTime = 0;
unsigned long currentTestDuration = 0;
unsigned long heatMatLowStartTime = 0;
unsigned long heatMatHighStartTime = 0;

float pastBaro = -1.0, pastHum = -1.0, pastTemp = -999.0;
unsigned long lastTrendSample = 0;
const char* baroTrendArrow = "=";
float kasTempHigh = -999.0, kasTempLow = 999.0;
float indoorTempHigh = -999.0, indoorTempLow = 999.0;
unsigned long lastHighLowReset = 0;

float stableLuxBase = 0.0;
bool lastLightStateOn = false;
unsigned long luxShiftStartTime = 0;
bool isLuxShiftPending = false;
bool isDisplayOff = false;
unsigned long darkStartTime = 0;


// =========================================================================
// 5. GLOBALE KLIMAAT & MARGE VARIABELEN
// =========================================================================
float kasDewPoint = -999.0, kasVpd = -999.0, indoorDewPoint = -999.0, indoorVpd = -999.0, currentPressure = 0.0, currentLuxValue = 0.0, outdoorVpd = -999.0;
bool moldRisk = false;
String moldReasonText = "";
float kasDpMargin = 0.0;
float indoorDpMargin = 0.0;
float outdoorDpMargin = 0.0;
String kasDpIcon = "🟢 (Veilig)";
String indoorDpIcon = "🟢 (Veilig)";
String indoorVpdIcon = "🟢";
String vpdStatusText = " `(⏳ Opstarten)`";
String dpMarginStatusText = " `(⏳ Kalibreren)`";

bool previousMoldRisk = false;
bool moldHistory[MOLD_SAMPLES] = {false};
int moldSampleIndex = 0;
bool moldHistoryFilled = false;
unsigned long lastMoldSampleTime = 0;

unsigned long lastBannerToggle = 0;
int bannerMode = 0;
unsigned long lastButtonPressTime = 0;

unsigned long lastBotCheckTime = 0;
unsigned long lastTelegramSentTime = 0;


// =========================================================================
// 6. VOCHTIGHEID & MIN/MAX STATISTIEKEN
// =========================================================================
float indoorHum = 0.0;
float indoorTemp = 0.0;
float kasHum = 0.0;
float kasTemp = 0.0;

float kasLowHum = 100.0;
float kasHighHum = 0.0;
float indoorLowHum = 100.0;
float indoorHighHum = 0.0;
float outdoorLowHum = 100.0;
float outdoorHighHum = 0.0;


// =========================================================================
// 7. VENTILATOREN, TACHO & PID RUNTIME VARIABELEN
// =========================================================================
volatile unsigned long rpmCountInt = 0;
volatile unsigned long rpmCountExt1 = 0;
volatile unsigned long rpmCountExt2 = 0;

int fanIntSpeed = 0, fanIntRPM = 0;
int fanExt1Speed = 0, fanExt1RPM = 0;
int fanExt2Speed = 0, fanExt2RPM = 0;
int fanIntPct = 0;
int fanExt1Pct = 0;
int fanExt2Pct = 0;

float pidOutput = 0.0; // <-- Voeg deze hier weer toe!
unsigned long lastPidTime = 0;
float pError = 0.0, iError = 0.0, dError = 0.0, lastError = 0.0;


// =========================================================================
// 8. ADVIEZEN & BESTURING
// =========================================================================
HouseVentState houseAdvice = HOUSE_CLOSED;
KasVentState kasAdvice = KAS_OFF;
HouseVentState previousHouseAdvice = HOUSE_CLOSED;
KasVentState previousKasAdvice = KAS_OFF;

String houseAdviceReason = "Klimaat in woning is stabiel.";
String kasAdviceReason = "Klimaat in de kas is in balans.";

unsigned long lastMqttUpdate = 0;
float previousHumForVelocity = -999.0;
unsigned long lastVelocityCheckTime = 0;
float humidityVelocity = 0.0;


// =========================================================================
// 9. HARDWARE & NETWERK OBJECTEN
// =========================================================================
unsigned long lastWifiLedBlink = 0;
bool wifiLedState = false;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiClientSecure telegramSslClient;
UniversalTelegramBot bot(BOT_TOKEN, telegramSslClient);

SSD1306Wire display(0x3c, PIN_SDA, PIN_SCL);