#include "sd_logger.h"
#include "audio_engine.h"
#include "globals.h"
#include "rtc_helper.h"

#include <esp_task_wdt.h>


// =============================================================================
//  File-local State
// =============================================================================

static File          audioFile;
static char          wavFileName[64];
static unsigned long lastDbSave           = 0;
static bool          splInitialized       = false;
static unsigned long recordStartTime      = 0;
static uint32_t      totalBytesWritten    = 0;
static uint32_t      sdErrorCount         = 0;    // consecutive SD write errors
static bool          sdHealthy            = true; // false → stop recording, retry remount
static unsigned long lastRemountAttemptMs = 0;


// =============================================================================
//  saveSPLValue  — append one CSV row
// =============================================================================

void saveSPLValue(float splValue) {
  if (!sdHealthy) return;

  GPSData g; LuxData l; UVData u; DHTData d;
  readGPSSafe(g); readLuxSafe(l); readUVSafe(u); readDHTSafe(d);

  char dtBuf[DATETIME_BUF_SIZE];
  getDateTimeStr(dtBuf, sizeof(dtBuf));

  MutexLock lock(sdMutex);
  File file = SD_MMC.open(dbFileName, FILE_APPEND);
  if (file) {
    file.printf(
        "%s,%.2f,%.2f,%.2f,%.1f,%.1f,%.7f,%.7f,%.1f,%.2f,%d,%.2f,%s,%s,%s,%s\n",
        dtBuf,
        splValue, l.value, u.index, d.temp, d.humidity,
        g.lat, g.lng, g.alt, g.speed, g.sats, g.hdop,
        g.valid ? "1" : "0", l.valid ? "1" : "0",
        u.valid ? "1" : "0", d.valid ? "1" : "0");
    file.close();
    sdErrorCount = 0;
  } else {
    sdErrorCount++;
  }
}


// =============================================================================
//  Private Helpers
// =============================================================================

// Update SPL from one audio block. Returns true when RMS window is complete.
static bool computeSPLBlock(AudioBlock *blk) {
  static float rmsAccumulator = 0.0f;
  static int   rmsCount       = 0;
  static float smoothSPL      = 0.0f;

  int numSamples = blk->byteCount / sizeof(int16_t);
  float sum = 0.0f;
  for (int i = 0; i < numSamples; i++) {
    float s = blk->samples[i] / 32768.0f;
    sum += s * s;
  }
  rmsAccumulator += sum / numSamples;
  rmsCount++;

  if (rmsCount < RMS_WINDOW) return false;

  float rms = sqrtf(rmsAccumulator / rmsCount);
  if (rms < 1e-5f) rms = 1e-5f;

  float rawSPL = 20.0f * log10f(rms) + MIC_OFFSET_DB;
  smoothSPL = splInitialized
                  ? EMA_ALPHA * rawSPL + (1.0f - EMA_ALPHA) * smoothSPL
                  : rawSPL;
  splInitialized   = true;
  currentSPL       = smoothSPL;
  rmsAccumulator   = 0.0f;
  rmsCount         = 0;
  return true;
}


// Try to start a new WAV recording. Returns true on success.
static bool tryStartRecording() {
  if (!sdHealthy) return false;

  // ── Check SD space ──────────────────────────────────────────────────────
  uint64_t freeMB;
  {
    MutexLock lock(sdMutex, 50);
    if (!lock) {
      LOG("WARN", "sdMutex timeout — skipping recording start");
      return false;
    }
    freeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  }

  if (freeMB < SD_MIN_FREE_MB) {
    LOG("WARN", "SD free < %dMB, skipping new recording", SD_MIN_FREE_MB);
    return false;
  }

  // ── Build path ──────────────────────────────────────────────────────────
  char dateFolder[DATEFOLDER_BUF_SIZE];
  char ts[TIMESTAMP_BUF_SIZE];
  getDateFolderStr(dateFolder, sizeof(dateFolder));
  getTimestampStr(ts, sizeof(ts));

  {
    MutexLock lock(sdMutex);
    if (!SD_MMC.exists(dateFolder))
      SD_MMC.mkdir(dateFolder);
  }

  snprintf(wavFileName, sizeof(wavFileName), "%s/audio_%s.wav", dateFolder, ts);

  // ── Open file + write WAV header ────────────────────────────────────────
  MutexLock lock(sdMutex);
  audioFile = SD_MMC.open(wavFileName, FILE_WRITE);
  if (audioFile) {
    WAVHeader header;
    createWAVHeader(header, 0);
    audioFile.write((uint8_t *)&header, sizeof(WAVHeader));
    isRecording       = true;
    recordStartTime   = millis();
    totalBytesWritten = 0;
    sdErrorCount      = 0;
    LOG("INFO", "[Core 0] Recording: %s", wavFileName);
  } else {
    sdErrorCount++;
  }
  return isRecording;
}


// Finalize WAV: update header, close file.
static void finalizeRecording() {
  {
    MutexLock lock(sdMutex);
    if (audioFile) {
      writeWAVHeader(audioFile, totalBytesWritten);
      audioFile.close();
    }
  }
  isRecording = false;
  LOG("INFO", "[Core 0] Recording complete (%.2f MB)", totalBytesWritten / 1048576.0f);
}


// Check SD health — if too many consecutive errors, mark unhealthy.
static void checkSDHealth() {
  if (sdErrorCount >= SD_ERROR_THRESHOLD && sdHealthy) {
    sdHealthy = false;
    lastRemountAttemptMs = millis();   // delay first remount by SD_REMOUNT_INTERVAL_MS
    LOG("ERROR", "SD card: %lu consecutive errors — stopping recording", sdErrorCount);
    if (isRecording) finalizeRecording();

    MutexLock lock(oledMutex, 10);
    if (lock) displayRecording = false;
  }
}


// Try to remount SD when unhealthy. Called periodically from sdProcessTask.
static void tryRemountSD() {
  if (sdHealthy) return;

  unsigned long now = millis();
  if (now - lastRemountAttemptMs < SD_REMOUNT_INTERVAL_MS) return;
  lastRemountAttemptMs = now;

  LOG("SD", "Attempting remount...");

  bool ok;
  {
    MutexLock lock(sdMutex, 100);
    if (!lock) return;
    SD_MMC.end();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    ok = SD_MMC.begin("/sdcard", true, false, 4000);
  }

  if (ok) {
    sdErrorCount = 0;
    sdHealthy    = true;
    LOG("SD", "Remount OK — recording resumed");
  } else {
    LOG("SD", "Remount failed — retry in %ds", SD_REMOUNT_INTERVAL_MS / 1000);
  }
}


// =============================================================================
//  sdProcessTask  (Core 0, Priority 1)
//
//  Receives AudioBlocks → WAV recording + SPL compute + CSV logging + OLED update
// =============================================================================

void sdProcessTask(void *parameter) {
  esp_task_wdt_add(NULL);
  LOG("INFO", "[Core 0] SD Process Task started");

  unsigned long lastSDCheck = 0;
  const unsigned long SD_CHECK_INTERVAL = 5000;

  while (1) {
    esp_task_wdt_reset();

    tryRemountSD();

    AudioBlock *blk = NULL;
    if (xQueueReceive(readyQueue, &blk, 100 / portTICK_PERIOD_MS) != pdTRUE) {
      // No block — periodic SD free space check
      if (millis() - lastSDCheck >= SD_CHECK_INTERVAL) {
        uint64_t sdFree = 0;
        bool haveFree = false;
        {
          MutexLock lock(sdMutex, 50);
          if (lock) {
            sdFree = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
            haveFree = true;
          }
        }
        if (haveFree) {
          MutexLock lock(oledMutex, 10);
          if (lock) displaySDFree = sdFree;
        }
        lastSDCheck = millis();
      }
      vTaskDelay(1);
      continue;
    }

    // ── Start / continue recording ─────────────────────────────────────
    if (!isRecording && sdHealthy)
      tryStartRecording();

    unsigned long now = millis();

    if (isRecording) {
      {
        MutexLock lock(sdMutex);
        if (audioFile) {
          size_t written = audioFile.write((uint8_t *)blk->samples, blk->byteCount);
          if (written == blk->byteCount) {
            totalBytesWritten += written;
            sdErrorCount = 0;
          } else {
            sdErrorCount++;
          }
        }
      }

      if (now - recordStartTime >= RECORD_TIME * 1000UL)
        finalizeRecording();
    }

    // ── Compute SPL ────────────────────────────────────────────────────
    computeSPLBlock(blk);

    // ── CSV log ────────────────────────────────────────────────────────
    if (now - lastDbSave >= DB_INTERVAL && splInitialized) {
      saveSPLValue(currentSPL);
      lastDbSave = now;
    }

    // ── Update OLED display vars ───────────────────────────────────────
    {
      MutexLock lock(oledMutex, 5);
      if (lock) {
        displaySPL           = currentSPL;
        displayRecording     = isRecording;
        displayRecordElapsed = isRecording ? (now - recordStartTime) / 1000 : 0;
      }
    }

    // ── SD health check ────────────────────────────────────────────────
    checkSDHealth();

    // ── Return block to pool ───────────────────────────────────────────
    xQueueSend(freeQueue, &blk, portMAX_DELAY);
    vTaskDelay(1);
  }
}
