/**
 * @file Config.cpp
 * @brief Implementatie van globale variabelen en objecten voor de kascontroller (ESP32-S3).
 */

#include "nono.h" 
#include "Config.h"

bool pendingTelegramAlert = false;

// --- NETWERK & SERVERS (Gekoppeld aan nono.h) ---
char ssid [] = SECRET_SSID;
char password [] = SECRET_PASS;
String openWeatherKey = API_KEY;
String cityID = CITY_ID;

char telbot[] = BOT_TOKEN;
char telid[] = CHAT_ID;

// --- SYSTEEM & NETWERK STATEN ---
unsigned long lastSuccessfulNetworkActivity = 0;
bool isConnected = false;

float espVcc = 0.0;
int wifiRSSI = 0;
uint32_t freeHeap = 0;

const char* BUFFER_FILE = "/data_buffer.txt";

// --- PID & VENTILATOR STREEFWAARDES ---
float pidSetPointVPD = 0.70;        
float kp = 2.0;                     
float ki = 0.1;                     
float kd = 0.5;                     

// --- SENSOR & KLIMAAT VARIABELEN ---
unsigned long totalKasDhtReads = 0;
unsigned long failedKasDhtReads = 0;
unsigned long lastValidKasDhtTime = 0;
unsigned long totalIndoorDhtReads = 0;
unsigned long failedIndoorDhtReads = 0;
unsigned long lastValidIndoorDhtTime = 0;

float kasSmoothedTemp = -999.0, kasSmoothedHum = -999.0, indoorSmoothedTemp = -999.0, indoorSmoothedHum = -999.0, displayedTemp = -999.0;
unsigned long lastTempUpdate = 0;

float outdoorTemp = 0.0, outdoorTempMax = 0.0, outdoorTempMin = 0.0, outdoorHumidity = 0.0, outdoorDewPoint = 0.0, outdoorPressure = 0.0;
bool outdoorMoldRisk = false, hasOutdoorAlert = false;
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

float kasDewPoint = -999.0, kasVpd = -999.0, indoorDewPoint = -999.0, indoorVpd = -999.0, currentPressure = 0.0, currentLuxValue = 0.0, outdoorVpd = -999.0;
bool moldRisk = false;
String moldReasonText = "";
String vpdStatusText = "";
String dpMarginStatusText = "";

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

// --- VOCHTIGHEID & MIN/MAX STATISTIEKEN ---
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

// Fysieke definitie van de tacho-tellers voor alle ventilatoren
volatile unsigned long rpmCountInt = 0;
volatile unsigned long rpmCountExt1 = 0;
volatile unsigned long rpmCountExt2 = 0;

int fanIntSpeed = 0, fanIntRPM = 0;
int fanExt1Speed = 0, fanExt1RPM = 0;
int fanExt2Speed = 0, fanExt2RPM = 0;
float pidOutput = 0.0;
unsigned long lastPidTime = 0;
float pError = 0.0, iError = 0.0, dError = 0.0, lastError = 0.0;

// // Ventilator variabelen
// volatile unsigned long rpmCountInt = 0, rpmCountExt = 0;
// // Fysieke definitie van de tacho-tellers voor alle ventilatoren
// volatile unsigned long rpmCountInt = 0;
// volatile unsigned long rpmCountExt1 = 0;
// volatile unsigned long rpmCountExt2 = 0;
// int fanIntSpeed = 0, fanIntRPM = 0;
// int fanExt1Speed = 0, fanExt1RPM = 0;
// int fanExt2Speed = 0, fanExt2RPM = 0;
// float pidOutput = 0.0;
// unsigned long lastPidTime = 0;
// float pError = 0.0, iError = 0.0, dError = 0.0, lastError = 0.0;

// Advies staten
HouseVentState houseAdvice = HOUSE_CLOSED;
KasVentState kasAdvice = OFF;
HouseVentState previousHouseAdvice = HOUSE_CLOSED;
KasVentState previousKasAdvice = OFF;

String houseAdviceReason = "Klimaat in woning is stabiel.";
String kasAdviceReason = "Klimaat in de kas is in balans.";

unsigned long lastMqttUpdate = 0;
float previousHumForVelocity = -999.0;
unsigned long lastVelocityCheckTime = 0;
float humidityVelocity = 0.0;

// --- HARDWARE & NETWERK OBJECTEN ---
// --- LED ---
unsigned long lastWifiLedBlink = 0;
bool wifiLedState = false;
// NETWERK OBJECTEN
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WiFiClientSecure telegramSslClient;
UniversalTelegramBot bot(BOT_TOKEN, telegramSslClient);

// Daadwerkelijke initialisatie van het display-object
SSD1306Wire display(0x3c, PIN_SDA, PIN_SCL);