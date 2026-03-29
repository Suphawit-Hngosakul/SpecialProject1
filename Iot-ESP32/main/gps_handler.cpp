#include "gps_handler.h"
#include "globals.h"
#include "rtc_helper.h"

// ========== GPS Task (Core 0) ==========
void gpsTask(void *parameter) {
  Serial.printf("[%s] [INFO] GPS Task started (UART2 RX=%d @ %d baud)\n",
                getDateTimeString().c_str(), GPS_RX_PIN, GPS_BAUD);
  while (1) {
    while (gpsSerial.available())
      gps.encode(gpsSerial.read());

    if (gps.location.isUpdated()) {
      if (xSemaphoreTake(gpsMutex, 10 / portTICK_PERIOD_MS)) {
        gpsValid = gps.location.isValid();
        if (gpsValid) {
          gpsLat = gps.location.lat();
          gpsLng = gps.location.lng();
          gpsAlt = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
          gpsSpeed = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
          gpsSats = gps.satellites.isValid() ? gps.satellites.value() : 0;
          gpsHdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f;
        }
        xSemaphoreGive(gpsMutex);
      }
    }

    if (xSemaphoreTake(oledMutex, 5 / portTICK_PERIOD_MS)) {
      displayGPSValid = gpsValid;
      displayLat = gpsLat;
      displayLng = gpsLng;
      displaySats = gpsSats;
      xSemaphoreGive(oledMutex);
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}
