#include "rtc_helper.h"
#include "globals.h"

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
  DateTime ts = rtc.now();
  Serial.printf("[INFO] RTC: %04d-%02d-%02d %02d:%02d:%02d\n", ts.year(),
                ts.month(), ts.day(), ts.hour(), ts.minute(), ts.second());
  return true;
}

String getTimestamp() {
  if (!rtcAvailable)
    return String(millis());
  DateTime ts = rtc.now();
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d", ts.year(), ts.month(),
           ts.day(), ts.hour(), ts.minute(), ts.second());
  return String(buf);
}

String getDateTimeString() {
  if (!rtcAvailable)
    return String(millis() / 1000);
  DateTime ts = rtc.now();
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", ts.year(),
           ts.month(), ts.day(), ts.hour(), ts.minute(), ts.second());
  return String(buf);
}

String getDateFolder() {
  if (!rtcAvailable)
    return "/logs";
  DateTime ts = rtc.now();
  char buf[16];
  snprintf(buf, sizeof(buf), "/%04d%02d%02d", ts.year(), ts.month(), ts.day());
  return String(buf);
}
