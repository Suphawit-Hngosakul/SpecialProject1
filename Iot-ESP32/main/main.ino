// ============================================================
//  V2fix.ino — ESP32 Multi-Sensor Logger (Refactored)
//  Main sketch: setup() + loop() only
//  All logic is split into modular .h/.cpp files
// ============================================================

#include "audio_engine.h"
#include "globals.h"
#include "gps_handler.h"
#include "oled_display.h"
#include "rtc_helper.h"
#include "sd_logger.h"
#include "sensors.h"
#include "wifi_ntp.h"

#include <esp_task_wdt.h>

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[INFO] Sound + GPS + Lux + UV + DHT22 Logger Starting...");

  // ---- I2C bus init ก่อนเป็นอันดับแรก (shared: OLED + RTC + BH1750) ----
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000); // 400kHz

  // ---- OLED ก่อนเลย — แสดงสถานะได้ตั้งแต่นาทีแรก ----
  initOLED();

  // ---- RTC ----
  oledShowStatus("Init RTC...");
  rtcAvailable = initRTC();
  if (!rtcAvailable)
    Serial.println("[WARN] Running without RTC (using millis)");
  oledShowStatus("RTC", rtcAvailable ? "DS3231 OK" : "No RTC (millis)");
  delay(700);

  // ---- WiFi + NTP ----
  oledShowStatus("WiFi", "Connecting...", WIFI_SSID);
  syncTimeNTP();
  oledShowStatus("NTP", "Time synced");
  delay(700);

  // ---- Sensors ----
  oledShowStatus("Sensors", "BH1750 / UV / DHT22");
  initBH1750();
  initUV();
  initDHT();

  // ---- Sensor warm-up countdown ----
  for (int i = SENSOR_WARMUP_SEC; i > 0; i--) {
    char wbuf[20];
    snprintf(wbuf, sizeof(wbuf), "Warmup: %ds...", i);
    oledShowStatus("Sensor Warmup", "Stabilizing...", wbuf);
    delay(1000);
  }
  oledShowStatus("Sensors", "Ready");
  delay(500);

  // ---- GPS UART ----
  oledShowStatus("GPS", "UART2 starting...");
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  Serial.printf("[%s] [INFO] GPS UART2 started (RX=%d TX=%d @ %d baud)\n",
                getDateTimeString().c_str(), GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);
  oledShowStatus("GPS", "UART2 Ready");
  delay(500);

  // ---- SD Card Init ----
  oledShowStatus("SD Card", "Initializing...");
  Serial.printf("[%s] [INFO] Initializing SD_MMC...\n",
                getDateTimeString().c_str());
  pinMode(SDMMC_DAT0_PIN, INPUT_PULLUP);
  pinMode(SDMMC_CMD_PIN,  INPUT_PULLUP);
  pinMode(SDMMC_CLK_PIN,  INPUT_PULLUP);
  delay(100);

  if (!SD_MMC.begin("/sdcard", true, false, 4000)) {
    Serial.printf("[%s] [ERROR] SD_MMC init failed! Restarting in 10s...\n",
                  getDateTimeString().c_str());
    oledShowStatus("SD Card", "ERROR!", "Restart 10s...");
    delay(10000);
    ESP.restart();
  }

  uint64_t freeSize =
      (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  Serial.printf("[%s] [INFO] SD OK. Free: %llu MB\n",
                getDateTimeString().c_str(), freeSize);

  char sdBuf[24];
  snprintf(sdBuf, sizeof(sdBuf), "Free: %llu MB", freeSize);
  oledShowStatus("SD Card", "Ready", sdBuf);
  delay(700);

  // ---- Create CSV Log File ----
  String timestamp = getTimestamp();
  String dateFolder = getDateFolder();
  if (!SD_MMC.exists(dateFolder.c_str()))
    SD_MMC.mkdir(dateFolder.c_str());

  snprintf(dbFileName, sizeof(dbFileName), "%s/spl_log_%s.csv",
           dateFolder.c_str(), timestamp.c_str());
  File file = SD_MMC.open(dbFileName, FILE_WRITE);
  if (file) {
    file.println("DateTime,dB_SPL,Lux,UV_Index,Temp_C,Humidity_pct,Latitude,"
                 "Longitude,Altitude_m,Speed_kmh,Satellites,HDOP,"
                 "GPS_Valid,Lux_Valid,UV_Valid,DHT_Valid");
    file.close();
    Serial.printf("[%s] [INFO] Log file: %s\n", getDateTimeString().c_str(),
                  dbFileName);
  }

  // ---- Create Semaphores & Queues ----
  oledShowStatus("System", "Creating queues...");
  freeQueue  = xQueueCreate(AUDIO_POOL_SIZE, sizeof(AudioBlock *));
  readyQueue = xQueueCreate(AUDIO_POOL_SIZE, sizeof(AudioBlock *));
  sdMutex    = xSemaphoreCreateMutex();
  oledMutex  = xSemaphoreCreateMutex();
  gpsMutex   = xSemaphoreCreateMutex();
  luxMutex   = xSemaphoreCreateMutex();
  uvMutex    = xSemaphoreCreateMutex();
  dhtMutex   = xSemaphoreCreateMutex();
  wireMutex  = xSemaphoreCreateMutex();

  if (!freeQueue || !readyQueue || !sdMutex || !oledMutex || !gpsMutex ||
      !luxMutex || !uvMutex || !dhtMutex || !wireMutex) {
    Serial.printf("[%s] [ERROR] Failed to create Queue/Mutex! Restarting...\n",
                  getDateTimeString().c_str());
    oledShowStatus("SYSTEM ERROR", "Queue/Mutex fail", "Restart 3s...");
    delay(3000);
    ESP.restart();
  }

  for (int i = 0; i < AUDIO_POOL_SIZE; i++) {
    AudioBlock *blk = &audioPool[i];
    xQueueSend(freeQueue, &blk, 0);
  }

  // ---- Start FreeRTOS Tasks ----
  oledShowStatus("Ready!", "Starting tasks...");
  delay(800);

  xTaskCreatePinnedToCore(micTask,       "micTask",       STACK_MIC,        NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(sdProcessTask, "sdProcessTask", STACK_SD_PROCESS, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(gpsTask,       "gpsTask",       STACK_GPS,        NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(luxTask,       "luxTask",       STACK_LUX,        NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(uvTask,        "uvTask",        STACK_UV,         NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(dhtTask,       "dhtTask",       STACK_DHT,        NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(oledTask,      "oledTask",      STACK_OLED,       NULL, 1, NULL, 0);

  Serial.printf("[%s] [INFO] All tasks started. OFFSET=%.1fdB | RMS_WIN=%d | "
                "EMA=%.2f | UV_GPIO=%d | DHT_GPIO=%d\n",
                getDateTimeString().c_str(), MIC_OFFSET_DB, RMS_WINDOW,
                EMA_ALPHA, UV_PIN, DHT_PIN);
}

// ========== Loop (Monitoring) ==========
void loop() {
  static unsigned long lastPrint = 0;

  if (millis() - lastPrint >= 10000) {
    uint64_t freeSize = 0;
    if (xSemaphoreTake(sdMutex, 100 / portTICK_PERIOD_MS)) {
      freeSize = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / 1048576;
      xSemaphoreGive(sdMutex);
    }

    GPSData g = readGPSSafe();
    LuxData l = readLuxSafe();
    UVData  u = readUVSafe();
    DHTData d = readDHTSafe();

    Serial.printf("[%s] [STAT] Heap:%u | SDFree:%lluMB | SPL:%.1fdB | "
                  "Lux:%.1flux | UV:%.2f | Temp:%.1f°C | Humi:%.1f%% | "
                  "GPS:%s(%.6f,%.6f) S:%d HDOP:%.1f | Rec:%s\n",
                  getDateTimeString().c_str(), ESP.getFreeHeap(), freeSize,
                  currentSPL, l.valid ? l.value : -1.0f, u.valid ? u.index : -1.0f,
                  d.valid ? d.temp : -99.0f, d.valid ? d.humidity : -1.0f,
                  g.valid ? "FIX " : "NoFx", g.lat, g.lng, g.sats, g.hdop,
                  isRecording ? "YES" : "No");

    lastPrint = millis();
  }
  delay(1000);
}
