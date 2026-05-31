#include "globals.h"

#include <esp_task_wdt.h>


// =============================================================================
//  Hardware Instances
// =============================================================================

i2s_chan_handle_t rx_handle = NULL;
HardwareSerial   gpsSerial(2);
TinyGPSPlus      gps;
BH1750           lightMeter(BH1750_ADDR);
DHT              dht(DHT_PIN, DHT_TYPE);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE);
RTC_DS3231       rtc;


// =============================================================================
//  Mutexes
// =============================================================================

SemaphoreHandle_t luxMutex;
SemaphoreHandle_t uvMutex;
SemaphoreHandle_t dhtMutex;
SemaphoreHandle_t oledMutex;
SemaphoreHandle_t gpsMutex;
SemaphoreHandle_t sdMutex;
SemaphoreHandle_t wireMutex;
SemaphoreHandle_t wifiMutex;


// =============================================================================
//  Sensor Raw State
// =============================================================================

float luxValue = 0.0f;
bool  luxValid = false;

float uvIndex = 0.0f;
bool  uvValid = false;

float dhtTemp = 0.0f, dhtHumidity = 0.0f;
bool  dhtValid = false;

double  gpsLat = 0.0, gpsLng = 0.0, gpsAlt = 0.0, gpsSpeed = 0.0;
uint8_t gpsSats = 0;
float   gpsHdop = 99.9f;
bool    gpsValid = false;


// =============================================================================
//  OLED Display State
// =============================================================================

float         displaySPL            = 0.0;
bool          displayRecording      = false;
unsigned long displayRecordElapsed  = 0;
uint64_t      displaySDFree         = 0;
double        displayLat = 0.0, displayLng = 0.0;
bool          displayGPSValid       = false;
uint8_t       displaySats           = 0;
float         displayLux            = 0.0f;
bool          displayLuxValid       = false;
float         displayUVIndex        = 0.0f;
bool          displayUVValid        = false;
float         displayTemp = 0.0f, displayHumidity = 0.0f;
bool          displayDHTValid       = false;


// =============================================================================
//  Misc Globals
// =============================================================================

bool rtcAvailable = false;

AudioBlock   audioPool[AUDIO_POOL_SIZE];
QueueHandle_t freeQueue;
QueueHandle_t readyQueue;

AudioBlock   streamPool[STREAM_POOL_SIZE];
QueueHandle_t streamFreeQueue;
QueueHandle_t streamReadyQueue;

char           dbFileName[64];
volatile float currentSPL  = 0.0f;
volatile bool  isRecording = false;


// =============================================================================
//  Thread-safe Sensor Reads
// =============================================================================

bool readGPSSafe(GPSData &out) {
  MutexLock l(gpsMutex, 10);
  if (!l) return false;
  out.lat   = gpsLat;    out.lng  = gpsLng;    out.alt   = gpsAlt;
  out.speed = gpsSpeed;  out.sats = gpsSats;
  out.hdop  = gpsHdop;   out.valid = gpsValid;
  return true;
}

bool readLuxSafe(LuxData &out) {
  MutexLock l(luxMutex, 10);
  if (!l) return false;
  out.value = luxValue;  out.valid = luxValid;
  return true;
}

bool readUVSafe(UVData &out) {
  MutexLock l(uvMutex, 10);
  if (!l) return false;
  out.index = uvIndex;  out.valid = uvValid;
  return true;
}

bool readDHTSafe(DHTData &out) {
  MutexLock l(dhtMutex, 10);
  if (!l) return false;
  out.temp = dhtTemp;  out.humidity = dhtHumidity;  out.valid = dhtValid;
  return true;
}


// =============================================================================
//  WDT-friendly Delay
// =============================================================================

void delayFeedWdt(uint32_t totalMs) {
  const uint32_t step = 1000;
  uint32_t left = totalMs;
  while (left > 0) {
    esp_task_wdt_reset();
    uint32_t d = (left > step) ? step : left;
    vTaskDelay(d / portTICK_PERIOD_MS);
    left -= d;
  }
}
