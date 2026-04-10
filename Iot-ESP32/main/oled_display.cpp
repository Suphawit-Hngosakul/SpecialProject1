#include "oled_display.h"
#include "globals.h"
#include "rtc_helper.h"

// ========== OLED Init ==========
// Wire ต้องถูก begin() ใน setup() ก่อนเรียกฟังก์ชันนี้
void initOLED() {
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(14, 24, "SPL+GPS Logger");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(30, 40, "Initializing...");
  u8g2.sendBuffer();
  Serial.println("[INFO] OLED SH1106 initialized.");
}

// ========== Setup Status Display ==========
// ใช้แสดงสถานะระหว่าง setup() — ไม่ใช้ mutex (ยังไม่มี task)
// Layout (128x64):
//  y= 8  [ SETUP ]
//  y=10  ─────────────────────────── (line)
//  y=26  <step> (font 6x10)
//  y=40  <detail> (font 5x7)
//  y=54  <extra> (font 5x7)
void oledShowStatus(const char *step, const char *detail, const char *extra) {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(0, 8, "[ SETUP ]");
  u8g2.drawHLine(0, 10, 128);

  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 26, step);

  if (detail) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 40, detail);
  }
  if (extra) {
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 54, extra);
  }

  u8g2.sendBuffer();
}

// ========== OLED Layout (5 rows) ==========
//  Row 1: Date+Time
//  Row 2: dB SPL + REC
//  Row 3: Lux + UV
//  Row 4: Temp + Humidity
//  Row 5: GPS coords+Sats

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
      DateTime oledNow(2000, 1, 1, 0, 0, 0); // fallback ถ้า mutex timeout
      bool timeOk = false;
      if (xSemaphoreTake(wireMutex, 50 / portTICK_PERIOD_MS)) {
        oledNow = rtc.now();
        timeOk = true;
        xSemaphoreGive(wireMutex);
      }
      if (timeOk) {
        snprintf(timeBuf, sizeof(timeBuf), "%02d/%02d/%04d %02d:%02d:%02d",
                 oledNow.day(), oledNow.month(), oledNow.year(), oledNow.hour(),
                 oledNow.minute(), oledNow.second());
      } else {
        snprintf(timeBuf, sizeof(timeBuf), "--/--/---- --:--:--");
      }
    } else {
      snprintf(timeBuf, sizeof(timeBuf), "uptime: %lus", millis() / 1000);
    }

    // ---- Row 2: dB SPL + REC ----
    if (rec) {
      snprintf(recSplBuf, sizeof(recSplBuf), "%.1fdB  REC(%lu/%ds)", spl,
               elapsed, RECORD_TIME);
    } else {
      snprintf(recSplBuf, sizeof(recSplBuf), "%.1fdB  IDLE %lluMB", spl, sdFree);
    }

    // ---- Row 3: Lux + UV ----
    if (luxOk)
      snprintf(luxUvBuf, sizeof(luxUvBuf), "Lux:%.1f UV:%.1f", lux, uvOk ? uvIdx : 0.0f);
    else
      snprintf(luxUvBuf, sizeof(luxUvBuf), "Lux:--- UV:%.1f", uvOk ? uvIdx : 0.0f);

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
    u8g2.drawStr(0, 19, recSplBuf);
    u8g2.drawHLine(0, 21, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 31, luxUvBuf);
    u8g2.drawHLine(0, 33, 128);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 43, dhtBuf);
    u8g2.drawHLine(0, 45, 128);

    u8g2.setFont(u8g2_font_4x6_tf);
    u8g2.drawStr(0, 54, gpsBuf);

    if (xSemaphoreTake(wireMutex, 100 / portTICK_PERIOD_MS)) {
      u8g2.sendBuffer();
      xSemaphoreGive(wireMutex);
    }

    vTaskDelay(OLED_REFRESH_MS / portTICK_PERIOD_MS);
  }
}
