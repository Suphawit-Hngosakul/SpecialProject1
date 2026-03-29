#include "oled_display.h"
#include "globals.h"
#include "rtc_helper.h"

// ========== OLED Init ==========
void initOLED() {
  if (!rtcAvailable)
    Wire.begin(I2C_SDA, I2C_SCL);
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 28, "SPL+GPS+Lux+UV+DHT");
  u8g2.drawStr(25, 45, "Starting...");
  u8g2.sendBuffer();
  Serial.println("[INFO] OLED SH1106 initialized.");
}

// ========== OLED Layout (5 rows, all small font) ==========
//  Row 1: Date+Time       (font 5x7)
//  Row 2: REC + dB SPL    (font 5x7)
//  Row 3: Lux + UV        (font 5x7)
//  Row 4: Temp + Humidity  (font 5x7)
//  Row 5: GPS coords+Sats (font 4x6)

void oledTask(void *parameter) {
  char timeBuf[24];
  char recSplBuf[28];
  char luxUvBuf[28];
  char dhtBuf[28];
  char gpsBuf[36];

  while (1) {
    float spl = 0;
    bool rec = false;
    unsigned long elapsed = 0;
    uint64_t sdFree = 0;
    double lat = 0, lng = 0;
    bool gpsOk = false;
    uint8_t sats = 0;
    float lux = 0;
    bool luxOk = false;
    float uvIdx = 0;
    bool uvOk = false;
    float temp = 0;
    float humi = 0;
    bool dhtOk = false;

    if (xSemaphoreTake(oledMutex, 20 / portTICK_PERIOD_MS)) {
      spl = displaySPL;
      rec = displayRecording;
      elapsed = displayRecordElapsed;
      sdFree = displaySDFree;
      lat = displayLat;
      lng = displayLng;
      gpsOk = displayGPSValid;
      sats = displaySats;
      lux = displayLux;
      luxOk = displayLuxValid;
      uvIdx = displayUVIndex;
      uvOk = displayUVValid;
      temp = displayTemp;
      humi = displayHumidity;
      dhtOk = displayDHTValid;
      xSemaphoreGive(oledMutex);
    }

    // ---- Row 1: Date + Time ----
    if (rtcAvailable) {
      DateTime oledNow = rtc.now();
      snprintf(timeBuf, sizeof(timeBuf), "%02d/%02d/%04d %02d:%02d:%02d",
               oledNow.day(), oledNow.month(), oledNow.year(), oledNow.hour(),
               oledNow.minute(), oledNow.second());
    } else {
      snprintf(timeBuf, sizeof(timeBuf), "uptime: %lus", millis() / 1000);
    }

    // ---- Row 2: REC + dB SPL ----
    if (rec) {
      snprintf(recSplBuf, sizeof(recSplBuf), "REC(%lu/%ds) %.1fdB", elapsed,
               RECORD_TIME, spl);
    } else {
      snprintf(recSplBuf, sizeof(recSplBuf), "IDLE %lluMB  %.1fdB", sdFree,
               spl);
    }

    // ---- Row 3: Lux + UV ----
    if (luxOk) {
      if (lux >= 10000.0f)
        snprintf(luxUvBuf, sizeof(luxUvBuf), "Lux:%.0f", lux);
      else if (lux >= 100.0f)
        snprintf(luxUvBuf, sizeof(luxUvBuf), "Lux:%.1f", lux);
      else
        snprintf(luxUvBuf, sizeof(luxUvBuf), "Lux:%.2f", lux);
    } else {
      snprintf(luxUvBuf, sizeof(luxUvBuf), "Lux:Err");
    }
    char uvPart[14];
    snprintf(uvPart, sizeof(uvPart), "  UV:%.1f", uvOk ? uvIdx : 0.0f);
    strncat(luxUvBuf, uvPart, sizeof(luxUvBuf) - strlen(luxUvBuf) - 1);

    // ---- Row 4: Temp + Humidity ----
    if (dhtOk) {
      snprintf(dhtBuf, sizeof(dhtBuf), "T:%.1f%cC  H:%.1f%%", temp, 0xB0, humi);
    } else {
      snprintf(dhtBuf, sizeof(dhtBuf), "T:--.-  H:--.-%%");
    }

    // ---- Row 5: GPS + Sats ----
    if (gpsOk) {
      snprintf(gpsBuf, sizeof(gpsBuf), "%.5f,%.5f S:%d", lat, lng, sats);
    } else {
      snprintf(gpsBuf, sizeof(gpsBuf), "GPS:NoFix S:%d", sats);
    }

    // ---- Draw OLED ----
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 7, timeBuf);
    u8g2.drawHLine(0, 9, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    if (rec && (millis() / 500) % 2 == 0) {
      u8g2.drawDisc(3, 16, 2);
      u8g2.drawStr(8, 19, recSplBuf);
    } else {
      u8g2.drawStr(0, 19, recSplBuf);
    }
    u8g2.drawHLine(0, 21, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 31, luxUvBuf);
    u8g2.drawHLine(0, 33, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 43, dhtBuf);
    u8g2.drawHLine(0, 45, 128);

    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(0, 54, gpsBuf);

    u8g2.sendBuffer();
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}
