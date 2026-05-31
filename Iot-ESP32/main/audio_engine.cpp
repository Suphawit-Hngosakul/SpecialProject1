#include "audio_engine.h"
#include "globals.h"
#include "rtc_helper.h"

#include <esp_task_wdt.h>


// =============================================================================
//  I2S Driver Install (static — internal to this file)
// =============================================================================

static bool i2s_install() {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = BUFFER_CNT;
  chan_cfg.dma_frame_num = BUFFER_LEN;

  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  if (err != ESP_OK) {
    LOG("ERROR", "i2s_new_channel failed: %d", err);
    return false;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)I2S_SCK,
          .ws   = (gpio_num_t)I2S_WS,
          .dout = I2S_GPIO_UNUSED,
          .din  = (gpio_num_t)I2S_SD,
          .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
      },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
  if (err != ESP_OK) {
    LOG("ERROR", "i2s_channel_init_std_mode failed: %d", err);
    return false;
  }
  return true;
}


// =============================================================================
//  I2S Teardown + Reinstall  (for error recovery)
// =============================================================================

static bool i2s_reinstall() {
  LOG("WARN", "[Core 1] Re-initializing I2S driver...");
  if (rx_handle) {
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    rx_handle = NULL;
  }
  if (!i2s_install()) return false;

  esp_err_t err = i2s_channel_enable(rx_handle);
  if (err != ESP_OK) {
    LOG("ERROR", "[Core 1] i2s_channel_enable failed after re-init: %d", err);
    return false;
  }
  LOG("INFO", "[Core 1] I2S re-initialized successfully");
  return true;
}


// =============================================================================
//  WAV Header Helpers
// =============================================================================

void createWAVHeader(WAVHeader &h, uint32_t dataSize) {
  h.dataSize = dataSize;
  h.fileSize = dataSize + sizeof(WAVHeader) - 8;
}

void writeWAVHeader(File &file, uint32_t dataSize) {
  WAVHeader h;
  createWAVHeader(h, dataSize);
  file.seek(0);
  file.write((uint8_t *)&h, sizeof(WAVHeader));
}


// =============================================================================
//  micTask  (Core 1, Priority 2)
//
//  Reads I2S → fills AudioBlock → sends to readyQueue (SD) + streamReadyQueue (TCP)
// =============================================================================

void micTask(void *parameter) {
  esp_task_wdt_add(NULL);

  LOG("INFO", "[Core 1] Starting I2S...");
  if (!i2s_install()) {
    LOG("ERROR", "[Core 1] I2S install failed — task abort");
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
    return;
  }

  esp_err_t enErr = i2s_channel_enable(rx_handle);
  if (enErr != ESP_OK) {
    LOG("ERROR", "[Core 1] i2s_channel_enable failed: %d", enErr);
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
    return;
  }

  int32_t *rawBuffer = (int32_t *)malloc(BUFFER_LEN * sizeof(int32_t));
  if (!rawBuffer) {
    LOG("ERROR", "[Core 1] malloc failed!");
    esp_task_wdt_delete(NULL);
    vTaskDelete(NULL);
    return;
  }

  LOG("INFO", "[Core 1] I2S Ready, reading audio...");

  size_t   bytesIn        = 0;
  uint32_t i2sErrorCount  = 0;
  static volatile uint32_t audioDropCount = 0;

  while (1) {
    esp_task_wdt_reset();

    esp_err_t result = i2s_channel_read(
        rx_handle, rawBuffer, BUFFER_LEN * sizeof(int32_t),
        &bytesIn, portMAX_DELAY);

    // ── I2S error recovery ───────────────────────────────────────────────
    if (result != ESP_OK || bytesIn == 0) {
      i2sErrorCount++;
      if (i2sErrorCount % 50 == 1)
        LOG("WARN", "[Core 1] I2S read error #%lu (err=%d)", i2sErrorCount, result);

      if (i2sErrorCount >= I2S_ERROR_THRESHOLD) {
        if (i2s_reinstall()) {
          i2sErrorCount = 0;
        } else {
          LOG("ERROR", "[Core 1] I2S re-init failed — waiting 5s");
          vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
      }
      vTaskDelay(1);
      continue;
    }

    i2sErrorCount = 0;
    int numSamples = bytesIn / sizeof(int32_t);

    // ── Send to SD pipeline ──────────────────────────────────────────────
    AudioBlock *blk = NULL;
    if (xQueueReceive(freeQueue, &blk, 0) == pdTRUE) {
      for (int i = 0; i < numSamples; i++)
        blk->samples[i] = (int16_t)(rawBuffer[i] >> 16);
      blk->byteCount = numSamples * sizeof(int16_t);

      // Fan-out: copy to TCP stream pool (best-effort, non-blocking)
      if (streamFreeQueue && streamReadyQueue) {
        AudioBlock *sblk = NULL;
        if (xQueueReceive(streamFreeQueue, &sblk, 0) == pdTRUE) {
          memcpy(sblk->samples, blk->samples, blk->byteCount);
          sblk->byteCount = blk->byteCount;
          if (xQueueSend(streamReadyQueue, &sblk, 0) != pdTRUE)
            xQueueSend(streamFreeQueue, &sblk, 0);
        }
      }

      xQueueSend(readyQueue, &blk, portMAX_DELAY);
    } else {
      audioDropCount++;
      if (audioDropCount % 100 == 1)
        LOG("WARN", "Audio drop #%lu (pool full)", audioDropCount);
    }

    vTaskDelay(1);
  }
}
