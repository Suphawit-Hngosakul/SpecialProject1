#ifndef GLOBALS_H
#define GLOBALS_H

#include "config.h"
#include <Arduino.h>
#include <BH1750.h>
#include <DHT.h>
#include <HardwareSerial.h>
#include <RTClib.h>
#include <SD_MMC.h>
#include <TinyGPSPlus.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/i2s_std.h>


// =============================================================================
//  Audio Block (shared by SD pool + TCP stream pool)
// =============================================================================

typedef struct {
  int16_t samples[BUFFER_LEN];
  size_t  byteCount;
} AudioBlock;


// =============================================================================
//  Sensor Data Structs (returned by readXxxSafe)
// =============================================================================

struct GPSData {
  double   lat   = 0, lng = 0, alt = 0, speed = 0;
  uint8_t  sats  = 0;
  float    hdop  = 99.9f;
  bool     valid = false;
};

struct LuxData {
  float value = 0.0f;
  bool  valid = false;
};

struct UVData {
  float index = 0.0f;
  bool  valid = false;
};

struct DHTData {
  float temp     = 0.0f;
  float humidity = 0.0f;
  bool  valid    = false;
};


// =============================================================================
//  WDT-friendly Delay
//  Splits a long delay into 1-second chunks and feeds the task watchdog
//  between chunks. Caller must already be registered with esp_task_wdt_add().
// =============================================================================

void delayFeedWdt(uint32_t totalMs);


// =============================================================================
//  Exponential Backoff Helper
//  Use:  Backoff b = BACKOFF_DEFAULT();
//        b.wait();     // sleep current step (WDT-friendly), then advance
//        b.reset();    // call after a successful operation
// =============================================================================

struct Backoff {
  uint32_t currentMs;
  uint32_t initialMs;
  uint32_t maxMs;
  float    multiplier;

  void     reset()  { currentMs = initialMs; }
  uint32_t nextMs() {
    uint32_t val = currentMs;
    uint32_t next = (uint32_t)(currentMs * multiplier);
    currentMs = (next < maxMs) ? next : maxMs;
    return val;
  }
  void wait() { delayFeedWdt(nextMs()); }
};

#define BACKOFF_DEFAULT()  { BACKOFF_INITIAL_MS, BACKOFF_INITIAL_MS, \
                             BACKOFF_MAX_MS, BACKOFF_MULTIPLIER }


// =============================================================================
//  Mutex RAII Lock
//  Releases automatically when the scope ends — no `xSemaphoreGive` needed.
//
//    { MutexLock l(myMutex, 100); if (l) { ... work ... } }   // 100ms timeout
//    { MutexLock l(myMutex);                ... work ...   }   // wait forever
// =============================================================================

class MutexLock {
public:
  MutexLock(SemaphoreHandle_t m, uint32_t timeoutMs)
    : m_(m), held_(xSemaphoreTake(m, timeoutMs / portTICK_PERIOD_MS) == pdTRUE) {}

  explicit MutexLock(SemaphoreHandle_t m)
    : m_(m), held_(xSemaphoreTake(m, portMAX_DELAY) == pdTRUE) {}

  ~MutexLock() { if (held_) xSemaphoreGive(m_); }

  explicit operator bool() const { return held_; }

  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

private:
  SemaphoreHandle_t m_;
  bool              held_;
};


// =============================================================================
//  Hardware Instances
// =============================================================================

extern i2s_chan_handle_t rx_handle;
extern HardwareSerial    gpsSerial;
extern TinyGPSPlus       gps;
extern BH1750            lightMeter;
extern DHT               dht;
extern U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2;
extern RTC_DS3231        rtc;


// =============================================================================
//  Mutexes
// =============================================================================

extern SemaphoreHandle_t luxMutex;
extern SemaphoreHandle_t uvMutex;
extern SemaphoreHandle_t dhtMutex;
extern SemaphoreHandle_t oledMutex;
extern SemaphoreHandle_t gpsMutex;
extern SemaphoreHandle_t sdMutex;
extern SemaphoreHandle_t wireMutex;
extern SemaphoreHandle_t wifiMutex;


// =============================================================================
//  Sensor Raw State  (written by sensor tasks under their mutex)
// =============================================================================

extern float   luxValue;
extern bool    luxValid;

extern float   uvIndex;
extern bool    uvValid;

extern float   dhtTemp, dhtHumidity;
extern bool    dhtValid;

extern double  gpsLat, gpsLng, gpsAlt, gpsSpeed;
extern uint8_t gpsSats;
extern float   gpsHdop;
extern bool    gpsValid;


// =============================================================================
//  OLED Display State  (written under oledMutex, read by oledTask)
// =============================================================================

extern float         displaySPL;
extern bool          displayRecording;
extern unsigned long displayRecordElapsed;
extern uint64_t      displaySDFree;
extern double        displayLat, displayLng;
extern bool          displayGPSValid;
extern uint8_t       displaySats;
extern float         displayLux;
extern bool          displayLuxValid;
extern float         displayUVIndex;
extern bool          displayUVValid;
extern float         displayTemp, displayHumidity;
extern bool          displayDHTValid;


// =============================================================================
//  Misc Globals
// =============================================================================

extern bool rtcAvailable;

// ── SD audio pool ────────────────────────────────────────────────────────────
extern AudioBlock   audioPool[AUDIO_POOL_SIZE];
extern QueueHandle_t freeQueue;
extern QueueHandle_t readyQueue;

// ── TCP stream pool (separate from SD pool) ─────────────────────────────────
extern AudioBlock   streamPool[STREAM_POOL_SIZE];
extern QueueHandle_t streamFreeQueue;
extern QueueHandle_t streamReadyQueue;

extern char           dbFileName[64];
extern volatile float currentSPL;
extern volatile bool  isRecording;


// =============================================================================
//  Thread-safe Sensor Reads
//  Returns true when mutex was acquired (data is fresh).
//  Returns false on mutex timeout — `out` keeps its default (zero/invalid).
// =============================================================================

bool readGPSSafe(GPSData &out);
bool readLuxSafe(LuxData &out);
bool readUVSafe(UVData  &out);
bool readDHTSafe(DHTData &out);

#endif // GLOBALS_H
