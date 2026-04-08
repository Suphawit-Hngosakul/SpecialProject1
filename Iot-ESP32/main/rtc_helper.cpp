#include "rtc_helper.h"
#include "globals.h"

// ---- wireMutex-safe rtc.now() ----
// ถ้า wireMutex ยัง NULL (ช่วง setup ก่อน task start) → เรียกตรงได้เลย ไม่มี race
// ถ้า wireMutex มีค่าแล้ว (tasks ทำงานอยู่) → ต้อง lock ก่อนเสมอ
static DateTime rtcNowSafe() {
  if (wireMutex) {
    DateTime ts(2000, 1, 1, 0, 0, 0); // fallback หาก mutex timeout
    if (xSemaphoreTake(wireMutex, 20 / portTICK_PERIOD_MS)) {
      ts = rtc.now();
      xSemaphoreGive(wireMutex);
    }
    return ts;
  }
  // wireMutex == NULL → อยู่ใน setup(), ไม่มี concurrent task
  return rtc.now();
}

bool initRTC() {
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!rtc.begin()) {
    Serial.println("[WARN] DS3231 RTC not found!");
    return false;
  }
  if (rtc.lostPower()) {
    Serial.println("[WARN] RTC lost power, setting default time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  DateTime ts = rtc.now(); // ปลอดภัย: initRTC() เรียกก่อน task ใดๆ start
  Serial.printf("[INFO] RTC: %04d-%02d-%02d %02d:%02d:%02d\n", ts.year(),
                ts.month(), ts.day(), ts.hour(), ts.minute(), ts.second());
  return true;
}

String getTimestamp() {
  if (!rtcAvailable)
    return String(millis());
  DateTime ts = rtcNowSafe();
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", ts.year(), ts.month(),
           ts.day(), ts.hour(), ts.minute(), ts.second());
  return String(buf);
}

String getDateTimeString() {
  if (!rtcAvailable)
    return String(millis() / 1000);
  DateTime ts = rtcNowSafe();
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", ts.year(),
           ts.month(), ts.day(), ts.hour(), ts.minute(), ts.second());
  return String(buf);
}

String getDateFolder() {
  if (!rtcAvailable)
    return "/logs";
  DateTime ts = rtcNowSafe();
  char buf[16];
  snprintf(buf, sizeof(buf), "/%04d%02d%02d", ts.year(), ts.month(), ts.day());
  return String(buf);
}
