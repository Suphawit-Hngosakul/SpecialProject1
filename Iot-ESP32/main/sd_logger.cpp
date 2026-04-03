#include "sd_logger.h"
#include "audio_engine.h"
#include "globals.h"
#include "rtc_helper.h"

// ========== Save SPL + GPS + UV + DHT to CSV ==========
void saveSPLValue(float splValue) {
  GPSData g = readGPSSafe();
  LuxData l = readLuxSafe();
  UVData  u = readUVSafe();
  DHTData d = readDHTSafe();

  if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
    File file = SD_MMC.open(dbFileName, FILE_APPEND);
    if (file) {
      file.printf(
          "%s,%.2f,%.2f,%.2f,%.1f,%.1f,%.7f,%.7f,%.1f,%.2f,%d,%.2f,%s,%s,%s,%s\n",
          getDateTimeString().c_str(), splValue,
          l.value, u.index, d.temp, d.humidity,
          g.lat, g.lng, g.alt, g.speed, g.sats, g.hdop,
          g.valid ? "1" : "0", l.valid ? "1" : "0",
          u.valid ? "1" : "0", d.valid ? "1" : "0");
      file.close();
    }
    xSemaphoreGive(sdMutex);
  }
}

// ========== Core 0: SD + SPL Processing Task ==========
void sdProcessTask(void *parameter) {
  Serial.printf("[%s] [INFO] [Core 0] SD Process Task started\n",
                getDateTimeString().c_str());

  unsigned long lastSDCheck = 0;
  const unsigned long SD_CHECK_INTERVAL = 5000;

  while (1) {
    AudioBlock *blk = NULL;
    if (xQueueReceive(readyQueue, &blk, 100 / portTICK_PERIOD_MS) == pdTRUE) {
      unsigned long currentTime = millis();

      // ---- Start New Recording ----
      if (!isRecording) {
        bool sdHasSpace = true;
        if (xSemaphoreTake(sdMutex, 50 / portTICK_PERIOD_MS)) {
          uint64_t freeMB =
              (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
          sdHasSpace = (freeMB >= SD_MIN_FREE_MB);
          xSemaphoreGive(sdMutex);
          if (!sdHasSpace) {
            Serial.printf(
                "[%s] [WARN] SD free < %dMB, skipping new recording\n",
                getDateTimeString().c_str(), SD_MIN_FREE_MB);
          }
        }

        if (sdHasSpace) {
          String timestamp = getTimestamp();
          String dateFolder = getDateFolder();

          if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
            if (!SD_MMC.exists(dateFolder.c_str()))
              SD_MMC.mkdir(dateFolder.c_str());
            xSemaphoreGive(sdMutex);
          }

          snprintf(wavFileName, sizeof(wavFileName), "%s/audio_%s.wav",
                   dateFolder.c_str(), timestamp.c_str());

          if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
            audioFile = SD_MMC.open(wavFileName, FILE_WRITE);
            if (audioFile) {
              WAVHeader header;
              createWAVHeader(header, 0);
              audioFile.write((uint8_t *)&header, sizeof(WAVHeader));
              isRecording = true;
              recordStartTime = currentTime;
              totalBytesWritten = 0;
              Serial.printf("[%s] [INFO] [Core 0] Recording: %s\n",
                            getDateTimeString().c_str(), wavFileName);
            }
            xSemaphoreGive(sdMutex);
          }
        }
      }

      // ---- Write Audio ----
      if (isRecording) {
        if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
          if (audioFile)
            totalBytesWritten +=
                audioFile.write((uint8_t *)blk->samples, blk->byteCount);
          xSemaphoreGive(sdMutex);
        }
      }

      // ---- Stop Recording ----
      if (isRecording &&
          (currentTime - recordStartTime >= RECORD_TIME * 1000UL)) {
        if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
          if (audioFile) {
            writeWAVHeader(audioFile, totalBytesWritten);
            audioFile.close();
          }
          xSemaphoreGive(sdMutex);
        }
        isRecording = false;
        Serial.printf("[%s] [INFO] [Core 0] Recording complete (%.2f MB)\n",
                      getDateTimeString().c_str(),
                      totalBytesWritten / 1048576.0);
      }

      // ---- SPL Calculation ----
      int numSamples = blk->byteCount / sizeof(int16_t);
      float sum = 0.0f;
      for (int i = 0; i < numSamples; i++) {
        float s = blk->samples[i] / 32768.0f;
        sum += s * s;
      }
      rmsAccumulator += sum / numSamples;
      rmsCount++;

      if (rmsCount >= RMS_WINDOW) {
        float rms = sqrt(rmsAccumulator / rmsCount);
        if (rms < 0.00001f)
          rms = 0.00001f;
        float rawSPL = 20.0f * log10f(rms) + MIC_OFFSET_DB;
        if (!splInitialized) {
          smoothSPL = rawSPL;
          splInitialized = true;
        } else
          smoothSPL = EMA_ALPHA * rawSPL + (1.0f - EMA_ALPHA) * smoothSPL;
        currentSPL = smoothSPL;
        rmsAccumulator = 0.0f;
        rmsCount = 0;
      }

      // ---- Save to CSV ----
      if (currentTime - lastDbSave >= DB_INTERVAL) {
        saveSPLValue(currentSPL);
        lastDbSave = currentTime;
      }

      // ---- Update OLED shared state ----
      if (xSemaphoreTake(oledMutex, 5 / portTICK_PERIOD_MS)) {
        displaySPL = currentSPL;
        displayRecording = isRecording;
        displayRecordElapsed =
            isRecording ? (currentTime - recordStartTime) / 1000 : 0;
        xSemaphoreGive(oledMutex);
      }

      // ---- Return block to freeQueue ----
      xQueueSend(freeQueue, &blk, portMAX_DELAY);
    }

    // ---- Periodic SD free space check ----
    if (millis() - lastSDCheck >= SD_CHECK_INTERVAL) {
      if (xSemaphoreTake(sdMutex, 50 / portTICK_PERIOD_MS)) {
        uint64_t sdFree =
            (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
        xSemaphoreGive(sdMutex);
        if (xSemaphoreTake(oledMutex, 10 / portTICK_PERIOD_MS)) {
          displaySDFree = sdFree;
          xSemaphoreGive(oledMutex);
        }
      }
      lastSDCheck = millis();
    }

    vTaskDelay(1);
  }
}
