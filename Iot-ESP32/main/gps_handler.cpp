#include "gps_handler.h"
#include "globals.h"
#include "rtc_helper.h"

#include <esp_task_wdt.h>


// =============================================================================
//  gpsTask  (Core 0) — parse NMEA from UART2, 50 ms cycle
// =============================================================================

void gpsTask(void *parameter) {
  esp_task_wdt_add(NULL);
  LOG("INFO", "GPS Task started (UART2 RX=%d @ %d baud)", GPS_RX_PIN, GPS_BAUD);

  unsigned long lastFixTime = 0;

  while (1) {
    esp_task_wdt_reset();

    while (gpsSerial.available())
      gps.encode(gpsSerial.read());

    if (gps.location.isUpdated()) {
      bool newValid = gps.location.isValid();
      MutexLock l(gpsMutex, 10);
      if (l) {
        gpsValid = newValid;
        if (gpsValid) {
          lastFixTime = millis();
          gpsLat   = gps.location.lat();
          gpsLng   = gps.location.lng();
          gpsAlt   = gps.altitude.isValid()    ? gps.altitude.meters()  : 0.0;
          gpsSpeed = gps.speed.isValid()       ? gps.speed.kmph()       : 0.0;
          gpsSats  = gps.satellites.isValid()  ? gps.satellites.value() : 0;
          gpsHdop  = gps.hdop.isValid()        ? gps.hdop.hdop()        : 99.9f;
        }
      }
    }

    // Fix timeout — mark invalid if no update within GPS_VALID_TIMEOUT_MS
    if (gpsValid && lastFixTime > 0 &&
        (millis() - lastFixTime >= GPS_VALID_TIMEOUT_MS)) {
      MutexLock l(gpsMutex, 10);
      if (l) gpsValid = false;
      LOG("WARN", "GPS fix timeout (%dms) — marking invalid", GPS_VALID_TIMEOUT_MS);
    }

    {
      MutexLock l(oledMutex, 5);
      if (l) {
        displayGPSValid = gpsValid;
        displayLat  = gpsLat;
        displayLng  = gpsLng;
        displaySats = gpsSats;
      }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}
