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

// ========== Audio Block (Pool Ring Buffer) ==========
typedef struct {
  int16_t samples[BUFFER_LEN];
  size_t byteCount;
} AudioBlock;

// ========== Sensor Data Structs (for mutex-safe reads) ==========
struct GPSData {
  double lat = 0, lng = 0, alt = 0, speed = 0;
  uint8_t sats = 0;
  float hdop = 99.9f;
  bool valid = false;
};
struct LuxData {
  float value = 0.0f;
  bool valid = false;
};
struct UVData {
  float index = 0.0f;
  bool valid = false;
};
struct DHTData {
  float temp = 0.0f;
  float humidity = 0.0f;
  bool valid = false;
};

// ========== Hardware Objects ==========
extern i2s_chan_handle_t rx_handle;
extern HardwareSerial gpsSerial;
extern TinyGPSPlus gps;
extern BH1750 lightMeter;
extern DHT dht;
extern U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2;
extern RTC_DS3231 rtc;

// ========== Mutexes ==========
extern SemaphoreHandle_t luxMutex;
extern SemaphoreHandle_t uvMutex;
extern SemaphoreHandle_t dhtMutex;
extern SemaphoreHandle_t oledMutex;
extern SemaphoreHandle_t gpsMutex;
extern SemaphoreHandle_t sdMutex;
extern SemaphoreHandle_t wireMutex;

// ========== Shared Sensor State ==========
extern float uvVoltage, uvIndex;
extern bool uvValid;

extern float dhtTemp, dhtHumidity;
extern bool dhtValid;

extern double gpsLat, gpsLng, gpsAlt, gpsSpeed;
extern uint8_t gpsSats;
extern float gpsHdop;
extern bool gpsValid;

extern float luxValue;
extern bool  luxValid;

// ========== OLED Display State ==========
extern float displaySPL;
extern bool displayRecording;
extern unsigned long displayRecordElapsed;
extern uint64_t displaySDFree;
extern double displayLat, displayLng;
extern bool displayGPSValid;
extern uint8_t displaySats;
extern float displayLux;
extern bool  displayLuxValid;
extern float displayUVIndex;
extern bool displayUVValid;
extern float displayTemp, displayHumidity;
extern bool displayDHTValid;

// ========== RTC State ==========
extern bool rtcAvailable;

// ========== Audio Pool ==========
extern AudioBlock audioPool[AUDIO_POOL_SIZE];
extern QueueHandle_t freeQueue;
extern QueueHandle_t readyQueue;
extern volatile uint32_t audioDropCount;

// ========== Recording State ==========
extern File audioFile;
extern char wavFileName[64];
extern char dbFileName[64];
extern volatile float currentSPL;   // เขียนโดย sdProcessTask, อ่านโดย loop() — volatile ป้องกัน compiler cache
extern unsigned long lastDbSave;
extern float rmsAccumulator;
extern int rmsCount;
extern float smoothSPL;
extern bool splInitialized;
extern volatile bool isRecording;    // เขียนโดย sdProcessTask, อ่านโดย loop() — volatile
extern unsigned long recordStartTime;
extern uint32_t totalBytesWritten;

// ========== Mutex-safe Sensor Read Helpers ==========
GPSData readGPSSafe();
LuxData readLuxSafe();
UVData  readUVSafe();
DHTData readDHTSafe();

#endif // GLOBALS_H
