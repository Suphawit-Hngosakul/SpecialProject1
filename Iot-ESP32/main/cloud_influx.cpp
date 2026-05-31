#include "cloud_influx.h"
#include "globals.h"
#include "rtc_helper.h"
#include "wifi_ntp.h"

#include <InfluxDbClient.h>
#include <esp_task_wdt.h>


// =============================================================================
//  Client + Point  (created once, lives for the entire task lifetime)
// =============================================================================

static InfluxDBClient influxClient(
    INFLUX_URL, INFLUX_ORG, INFLUX_BUCKET, INFLUX_TOKEN);

static Point sensor("sensor_data");


// =============================================================================
//  influxTask  (Core 0)
//
//  Reads all sensor globals via readXxxSafe() → writes to InfluxDB every
//  INFLUX_INTERVAL_MS.  Uses exponential backoff on consecutive failures.
// =============================================================================

void influxTask(void *parameter) {
  esp_task_wdt_add(NULL);
  LOG("INFLUX", "Task started → %s", INFLUX_URL);

  // Wait for WiFi to settle after NTP sync
  vTaskDelay(3000 / portTICK_PERIOD_MS);

  // Device tag — set once, clearFields() does not remove tags
  sensor.addTag("device", DEVICE_ID);

  // Batch 5 records before sending → fewer HTTP requests
  influxClient.setWriteOptions(WriteOptions().batchSize(5));
  influxClient.setHTTPOptions(HTTPOptions().httpReadTimeout(INFLUX_HTTP_TIMEOUT_MS));

  Backoff backoff = BACKOFF_DEFAULT();

  while (1) {
    esp_task_wdt_reset();

    if (!ensureWiFiConnected()) {
      LOG("INFLUX", "WiFi unavailable — skip");
      backoff.wait();
      continue;
    }

    // ── Read sensor data (skip send if mutex timeout) ────────────────────
    GPSData g; LuxData l; UVData u; DHTData d;
    readGPSSafe(g); readLuxSafe(l); readUVSafe(u); readDHTSafe(d);

    // ── Build point ──────────────────────────────────────────────────────
    sensor.clearFields();
    sensor.addField("spl",    currentSPL);
    sensor.addField("lux",    l.valid ? l.value    : 0.0f);
    sensor.addField("lux_ok", l.valid ? 1 : 0);
    sensor.addField("uv",     u.valid ? u.index    : 0.0f);
    sensor.addField("uv_ok",  u.valid ? 1 : 0);
    sensor.addField("temp",   d.valid ? d.temp     : 0.0f);
    sensor.addField("humi",   d.valid ? d.humidity : 0.0f);
    sensor.addField("dht_ok", d.valid ? 1 : 0);
    sensor.addField("lat",    g.lat);
    sensor.addField("lng",    g.lng);
    sensor.addField("alt",    g.alt);
    sensor.addField("speed",  g.speed);
    sensor.addField("sats",   (int)g.sats);
    sensor.addField("hdop",   g.hdop);
    sensor.addField("gps_ok", g.valid ? 1 : 0);

    // ── Write (library handles batch + retry internally) ─────────────────
    if (influxClient.writePoint(sensor)) {
      backoff.reset();
      vTaskDelay(INFLUX_INTERVAL_MS / portTICK_PERIOD_MS);
    } else {
      LOG("INFLUX", "Write failed: %s",
          influxClient.getLastErrorMessage().c_str());
      backoff.wait();
    }
  }
}
