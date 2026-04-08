#include "audio_engine.h"
#include "globals.h"
#include "rtc_helper.h"

// ========== I2S (New API for ESP-IDF 5.x / Arduino Core 3.x) ==========
void i2s_install() {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = BUFFER_CNT;
  chan_cfg.dma_frame_num = BUFFER_LEN;
  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] i2s_new_channel failed: %d\n", err);
    return;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = (gpio_num_t)I2S_SCK,
              .ws = (gpio_num_t)I2S_WS,
              .dout = I2S_GPIO_UNUSED,
              .din = (gpio_num_t)I2S_SD,
              .invert_flags = {.mclk_inv = false,
                               .bclk_inv = false,
                               .ws_inv = false},
          },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  err = i2s_channel_init_std_mode(rx_handle, &std_cfg);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] i2s_channel_init_std_mode failed: %d\n", err);
  }
}

// ========== WAV Helpers ==========
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

// ========== Core 1: Mic Task ==========
void micTask(void *parameter) {
  Serial.printf("[%s] [INFO] [Core 1] Starting I2S...\n",
                getDateTimeString().c_str());
  i2s_install();

  // Guard: ถ้า i2s_install() ล้มเหลว rx_handle จะยัง NULL
  if (rx_handle == NULL) {
    Serial.printf("[%s] [ERROR] [Core 1] I2S handle null — task abort\n",
                  getDateTimeString().c_str());
    vTaskDelete(NULL);
    return;
  }

  esp_err_t enErr = i2s_channel_enable(rx_handle);
  if (enErr != ESP_OK) {
    Serial.printf("[%s] [ERROR] [Core 1] i2s_channel_enable failed: %d\n",
                  getDateTimeString().c_str(), enErr);
    vTaskDelete(NULL);
    return;
  }

  int32_t *rawBuffer = (int32_t *)malloc(BUFFER_LEN * sizeof(int32_t));
  if (!rawBuffer) {
    Serial.printf("[%s] [ERROR] [Core 1] malloc failed!\n",
                  getDateTimeString().c_str());
    vTaskDelete(NULL);
    return;
  }

  Serial.printf("[%s] [INFO] [Core 1] I2S Ready, reading audio...\n",
                getDateTimeString().c_str());

  size_t bytesIn = 0;
  while (1) {
    esp_err_t result =
        i2s_channel_read(rx_handle, rawBuffer, BUFFER_LEN * sizeof(int32_t),
                         &bytesIn, portMAX_DELAY);
    if (result == ESP_OK && bytesIn > 0) {
      int numSamples = bytesIn / sizeof(int32_t);

      AudioBlock *blk = NULL;
      if (xQueueReceive(freeQueue, &blk, 0) == pdTRUE) {
        for (int i = 0; i < numSamples; i++) {
          blk->samples[i] = (int16_t)(rawBuffer[i] >> 16);
        }
        blk->byteCount = numSamples * sizeof(int16_t);
        xQueueSend(readyQueue, &blk, portMAX_DELAY);
      } else {
        audioDropCount++;
        if (audioDropCount % 100 == 1) {
          Serial.printf("[%s] [WARN] Audio drop #%lu (pool full)\n",
                        getDateTimeString().c_str(), audioDropCount);
        }
      }
    }
    vTaskDelay(1);
  }
  // หมายเหตุ: free(rawBuffer) ไม่ถูกเรียกในการทำงานปกติ (while loop ไม่มี exit)
  // ถ้าต้องการรองรับ task cancellation ในอนาคต ให้เพิ่ม vTaskDelete notification ที่นี่
}
