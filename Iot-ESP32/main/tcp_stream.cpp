#include "tcp_stream.h"
#include "globals.h"
#include "rtc_helper.h"
#include "wifi_ntp.h"

#include <WiFi.h>
#include <esp_task_wdt.h>


// =============================================================================
//  tcpStreamTask  (Core 0)
//
//  Streams raw PCM audio blocks to EC2 via TCP.
//  Uses its own memory pool (streamPool) — does not compete with SD pipeline.
// =============================================================================

void tcpStreamTask(void *parameter) {
  esp_task_wdt_add(NULL);
  LOG("STREAM", "Task started → %s:%d", EC2_IP, EC2_PORT);

  WiFiClient client;
  Backoff backoff = BACKOFF_DEFAULT();

  while (1) {
    esp_task_wdt_reset();

    // ── Wait for WiFi ────────────────────────────────────────────────────
    if (!ensureWiFiConnected()) {
      backoff.wait();
      continue;
    }

    // ── Connect to EC2 ───────────────────────────────────────────────────
    if (!client.connect(EC2_IP, EC2_PORT)) {
      LOG("STREAM", "Connect failed");
      backoff.wait();
      continue;
    }

    backoff.reset();
    client.setTimeout(TCP_WRITE_TIMEOUT_MS / 1000);   // WiFiClient uses seconds
    LOG("STREAM", "Connected to %s:%d", EC2_IP, EC2_PORT);

    // ── Stream loop ──────────────────────────────────────────────────────
    while (client.connected()) {
      esp_task_wdt_reset();

      AudioBlock *blk = NULL;
      if (xQueueReceive(streamReadyQueue, &blk, 200 / portTICK_PERIOD_MS) != pdTRUE)
        continue;

      // Write with timeout — prevent infinite blocking
      const uint8_t *ptr = (const uint8_t *)blk->samples;
      size_t remaining   = blk->byteCount;
      unsigned long writeStart = millis();
      bool writeOk = true;

      while (remaining > 0 && client.connected()) {
        if (millis() - writeStart >= TCP_WRITE_TIMEOUT_MS) {
          LOG("STREAM", "Write timeout (%dms) — reconnecting", TCP_WRITE_TIMEOUT_MS);
          writeOk = false;
          break;
        }
        size_t written = client.write(ptr, remaining);
        if (written == 0) { writeOk = false; break; }
        ptr       += written;
        remaining -= written;
      }

      xQueueSend(streamFreeQueue, &blk, portMAX_DELAY);

      if (!writeOk) {
        LOG("STREAM", "Write failed — reconnecting");
        break;
      }
    }

    // ── Drain stale audio before reconnect ───────────────────────────────
    {
      AudioBlock *blk = NULL;
      while (xQueueReceive(streamReadyQueue, &blk, 0) == pdTRUE)
        xQueueSend(streamFreeQueue, &blk, 0);
    }

    client.stop();
    LOG("STREAM", "Disconnected");
    backoff.wait();
  }
}
