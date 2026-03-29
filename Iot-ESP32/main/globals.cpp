#include "globals.h"

// ========== Hardware Objects ==========
i2s_chan_handle_t rx_handle = NULL;
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
BH1750 lightMeter(BH1750_ADDR);
DHT dht(DHT_PIN, DHT_TYPE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE);
RTC_DS3231 rtc;

// ========== Mutexes ==========
SemaphoreHandle_t uvMutex;
SemaphoreHandle_t dhtMutex;
SemaphoreHandle_t oledMutex;
SemaphoreHandle_t gpsMutex;
SemaphoreHandle_t luxMutex;
SemaphoreHandle_t sdMutex;

// ========== Shared Sensor State ==========
float uvVoltage = 0.0f, uvIndex = 0.0f;
bool uvValid = false;

float dhtTemp = 0.0f, dhtHumidity = 0.0f;
bool dhtValid = false;

double gpsLat = 0.0, gpsLng = 0.0, gpsAlt = 0.0, gpsSpeed = 0.0;
uint8_t gpsSats = 0;
float gpsHdop = 99.9f;
bool gpsValid = false;

float luxValue = 0.0f;
bool luxValid = false;

// ========== OLED Display State ==========
float displaySPL = 0.0;
bool displayRecording = false;
unsigned long displayRecordElapsed = 0;
uint64_t displaySDFree = 0;
double displayLat = 0.0, displayLng = 0.0;
bool displayGPSValid = false;
uint8_t displaySats = 0;
float displayLux = 0.0;
bool displayLuxValid = false;
float displayUVIndex = 0.0f;
bool displayUVValid = false;
float displayTemp = 0.0f, displayHumidity = 0.0f;
bool displayDHTValid = false;

// ========== RTC State ==========
bool rtcAvailable = false;

// ========== Audio Pool ==========
AudioBlock audioPool[AUDIO_POOL_SIZE];
QueueHandle_t freeQueue;
QueueHandle_t readyQueue;
volatile uint32_t audioDropCount = 0;

// ========== Recording State ==========
File audioFile;
char wavFileName[64];
char dbFileName[64];
float currentSPL = 0.0;
unsigned long lastDbSave = 0;
float rmsAccumulator = 0.0f;
int rmsCount = 0;
float smoothSPL = 0.0f;
bool splInitialized = false;
bool isRecording = false;
unsigned long recordStartTime = 0;
uint32_t totalBytesWritten = 0;

// ========== Mutex-safe Sensor Read Helpers ==========
GPSData readGPSSafe() {
  GPSData d;
  if (xSemaphoreTake(gpsMutex, 10 / portTICK_PERIOD_MS)) {
    d.lat = gpsLat;  d.lng = gpsLng;  d.alt = gpsAlt;
    d.speed = gpsSpeed;  d.sats = gpsSats;
    d.hdop = gpsHdop;  d.valid = gpsValid;
    xSemaphoreGive(gpsMutex);
  }
  return d;
}

LuxData readLuxSafe() {
  LuxData d;
  if (xSemaphoreTake(luxMutex, 10 / portTICK_PERIOD_MS)) {
    d.value = luxValue;  d.valid = luxValid;
    xSemaphoreGive(luxMutex);
  }
  return d;
}

UVData readUVSafe() {
  UVData d;
  if (xSemaphoreTake(uvMutex, 10 / portTICK_PERIOD_MS)) {
    d.index = uvIndex;  d.valid = uvValid;
    xSemaphoreGive(uvMutex);
  }
  return d;
}

DHTData readDHTSafe() {
  DHTData d;
  if (xSemaphoreTake(dhtMutex, 10 / portTICK_PERIOD_MS)) {
    d.temp = dhtTemp;  d.humidity = dhtHumidity;  d.valid = dhtValid;
    xSemaphoreGive(dhtMutex);
  }
  return d;
}
