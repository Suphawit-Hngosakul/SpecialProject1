#include "audio_engine.h"
#include "cloud_influx.h"
#include "tcp_stream.h"
#include "globals.h"
#include "gps_handler.h"
#include "oled_display.h"
#include "rtc_helper.h"
#include "sd_logger.h"
#include "sensors.h"
#include "wifi_ntp.h"

#include <esp_task_wdt.h>


// =============================================================================
//  Task Table
//  Add a new FreeRTOS task: declare its entry function in the matching header,
//  then append one row here. Order doesn't matter — they all start at boot.
// =============================================================================

struct TaskSpec {
  TaskFunction_t fn;
  const char *   name;
  uint32_t       stack;
  UBaseType_t    priority;
  BaseType_t     core;
};

static const TaskSpec kTasks[] = {
  // fn,             name,             stack,            prio, core
  { micTask,         "micTask",        STACK_MIC,        2,    1 },
  { sdProcessTask,   "sdProcessTask",  STACK_SD_PROCESS, 1,    0 },
  { gpsTask,         "gpsTask",        STACK_GPS,        1,    0 },
  { luxTask,         "luxTask",        STACK_LUX,        1,    0 },
  { uvTask,          "uvTask",         STACK_UV,         1,    0 },
  { dhtTask,         "dhtTask",        STACK_DHT,        1,    0 },
  { oledTask,        "oledTask",       STACK_OLED,       1,    0 },
  { influxTask,      "influxTask",     STACK_INFLUX,     1,    0 },
  { tcpStreamTask,   "tcpStreamTask",  STACK_TCP_STREAM, 1,    0 },
};


// =============================================================================
//  Init Helpers
// =============================================================================

static bool initSDCard() {
  oledShowStatus("SD Card", "Initializing...");
  LOG("INFO", "Initializing SD_MMC...");

  pinMode(SDMMC_DAT0_PIN, INPUT_PULLUP);
  pinMode(SDMMC_CMD_PIN,  INPUT_PULLUP);
  pinMode(SDMMC_CLK_PIN,  INPUT_PULLUP);
  delay(100);

  if (!SD_MMC.begin("/sdcard", true, false, 4000)) {
    LOG("ERROR", "SD_MMC init failed! Restarting in 10s...");
    oledShowStatus("SD Card", "ERROR!", "Restart 10s...");
    delay(10000);
    ESP.restart();
  }

  uint64_t freeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  LOG("INFO", "SD OK. Free: %llu MB", freeMB);

  char buf[24];
  snprintf(buf, sizeof(buf), "Free: %llu MB", freeMB);
  oledShowStatus("SD Card", "Ready", buf);
  delay(700);
  return true;
}


static void initQueuesAndMutexes() {
  oledShowStatus("System", "Creating queues...");

  freeQueue        = xQueueCreate(AUDIO_POOL_SIZE,  sizeof(AudioBlock *));
  readyQueue       = xQueueCreate(AUDIO_POOL_SIZE,  sizeof(AudioBlock *));
  streamFreeQueue  = xQueueCreate(STREAM_POOL_SIZE, sizeof(AudioBlock *));
  streamReadyQueue = xQueueCreate(STREAM_POOL_SIZE, sizeof(AudioBlock *));

  sdMutex   = xSemaphoreCreateMutex();
  oledMutex = xSemaphoreCreateMutex();
  gpsMutex  = xSemaphoreCreateMutex();
  luxMutex  = xSemaphoreCreateMutex();
  uvMutex   = xSemaphoreCreateMutex();
  dhtMutex  = xSemaphoreCreateMutex();
  wireMutex = xSemaphoreCreateMutex();
  wifiMutex = xSemaphoreCreateMutex();

  if (!freeQueue || !readyQueue || !streamFreeQueue || !streamReadyQueue ||
      !sdMutex || !oledMutex || !gpsMutex ||
      !luxMutex || !uvMutex || !dhtMutex || !wireMutex || !wifiMutex) {
    LOG("ERROR", "Failed to create Queue/Mutex! Restarting...");
    oledShowStatus("SYSTEM ERROR", "Queue/Mutex fail", "Restart 3s...");
    delay(3000);
    ESP.restart();
  }

  for (int i = 0; i < AUDIO_POOL_SIZE; i++) {
    AudioBlock *blk = &audioPool[i];
    xQueueSend(freeQueue, &blk, 0);
  }
  for (int i = 0; i < STREAM_POOL_SIZE; i++) {
    AudioBlock *blk = &streamPool[i];
    xQueueSend(streamFreeQueue, &blk, 0);
  }
}


// =============================================================================
//  Setup
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[INFO] Sound + GPS + Lux + UV + DHT22 Logger Starting...");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  initOLED();

  // ── RTC ──────────────────────────────────────────────────────────────────
  oledShowStatus("Init RTC...");
  rtcAvailable = initRTC();
  if (!rtcAvailable) Serial.println("[WARN] Running without RTC (using millis)");
  oledShowStatus("RTC", rtcAvailable ? "DS3231 OK" : "No RTC (millis)");
  delay(700);

  // ── WiFi + NTP ───────────────────────────────────────────────────────────
  oledShowStatus("WiFi", "Connecting...", WIFI_SSID_1);
  const bool online = syncTimeNTP();
  if (online) {
    oledShowStatus("Network", "ONLINE", "Time synced");
  } else {
    oledShowStatus("Network", "OFFLINE", "No WiFi found");
    LOG("INFO", "Booted OFFLINE — cloud tasks will retry until WiFi recovers");
  }
  delay(700);

  // ── Sensors ──────────────────────────────────────────────────────────────
  oledShowStatus("Sensors", "BH1750 / UV / DHT22");
  initBH1750();
  initUV();
  initDHT();

  for (int i = SENSOR_WARMUP_SEC; i > 0; i--) {
    char buf[20];
    snprintf(buf, sizeof(buf), "Warmup: %ds...", i);
    oledShowStatus("Sensor Warmup", "Stabilizing...", buf);
    delay(1000);
  }
  oledShowStatus("Sensors", "Ready");
  delay(500);

  // ── GPS ──────────────────────────────────────────────────────────────────
  oledShowStatus("GPS", "UART2 starting...");
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  LOG("INFO", "GPS UART2 started (RX=%d TX=%d @ %d baud)",
      GPS_RX_PIN, GPS_TX_PIN, GPS_BAUD);
  oledShowStatus("GPS", "UART2 Ready");
  delay(500);

  // ── SD Card ──────────────────────────────────────────────────────────────
  initSDCard();

  // ── CSV log file ─────────────────────────────────────────────────────────
  char timestamp[TIMESTAMP_BUF_SIZE];
  char dateFolder[DATEFOLDER_BUF_SIZE];
  getTimestampStr(timestamp, sizeof(timestamp));
  getDateFolderStr(dateFolder, sizeof(dateFolder));

  if (!SD_MMC.exists(dateFolder))
    SD_MMC.mkdir(dateFolder);

  snprintf(dbFileName, sizeof(dbFileName), "%s/spl_log_%s.csv",
           dateFolder, timestamp);
  File file = SD_MMC.open(dbFileName, FILE_WRITE);
  if (file) {
    file.println("DateTime,dB_SPL,Lux,UV_Index,Temp_C,Humidity_pct,Latitude,"
                 "Longitude,Altitude_m,Speed_kmh,Satellites,HDOP,"
                 "GPS_Valid,Lux_Valid,UV_Valid,DHT_Valid");
    file.close();
    LOG("INFO", "Log file: %s", dbFileName);
  }

  // ── Queues / Mutexes ────────────────────────────────────────────────────
  initQueuesAndMutexes();

  // ── Watchdog Timer ───────────────────────────────────────────────────────
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms     = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  esp_task_wdt_reconfigure(&wdtCfg);   // panic + reboot on timeout

  // ── Launch Tasks ─────────────────────────────────────────────────────────
  // Cloud tasks (influx/tcpStream) self-recover via ensureWiFiConnected() + backoff,
  // so they always start even if WiFi was unavailable at boot.
  oledShowStatus("Ready!", "Starting tasks...");
  delay(800);

  for (const TaskSpec &t : kTasks) {
    xTaskCreatePinnedToCore(t.fn, t.name, t.stack, NULL, t.priority, NULL, t.core);
  }

  LOG("INFO", "All tasks started. OFFSET=%.1fdB | RMS_WIN=%d | EMA=%.2f | UV_GPIO=%d | DHT_GPIO=%d",
      MIC_OFFSET_DB, RMS_WINDOW, EMA_ALPHA, UV_PIN, DHT_PIN);
}


// =============================================================================
//  Loop  — periodic status print (runs on Core 1)
// =============================================================================

void loop() {
  static unsigned long lastPrint = 0;

  if (millis() - lastPrint >= 10000) {
    uint64_t freeSize = 0;
    {
      MutexLock lock(sdMutex, 100);
      if (lock) freeSize = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / 1048576;
    }

    GPSData g; LuxData l; UVData u; DHTData d;
    readGPSSafe(g); readLuxSafe(l); readUVSafe(u); readDHTSafe(d);

    LOG("STAT", "Heap:%u | SDFree:%lluMB | SPL:%.1fdB | "
        "Lux:%.1flux | UV:%.2f | Temp:%.1f°C | Humi:%.1f%% | "
        "GPS:%s(%.6f,%.6f) S:%d HDOP:%.1f | Rec:%s",
        ESP.getFreeHeap(), freeSize,
        currentSPL, l.valid ? l.value : -1.0f, u.valid ? u.index : -1.0f,
        d.valid ? d.temp : -99.0f, d.valid ? d.humidity : -1.0f,
        g.valid ? "FIX " : "NoFx", g.lat, g.lng, g.sats, g.hdop,
        isRecording ? "YES" : "No");

    lastPrint = millis();
  }
  delay(1000);
}
