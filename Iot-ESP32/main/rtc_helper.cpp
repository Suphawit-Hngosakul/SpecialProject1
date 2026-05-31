#include "rtc_helper.h"
#include "globals.h"


// =============================================================================
//  Private — read RTC under wireMutex
// =============================================================================

static DateTime rtcNowSafe() {
  // During setup() before initQueuesAndMutexes(), wireMutex is NULL.
  // No concurrent I2C access exists yet, so read directly.
  if (!wireMutex) return rtc.now();

  DateTime ts(2000, 1, 1, 0, 0, 0);
  MutexLock lock(wireMutex, 100);
  if (lock) ts = rtc.now();
  return ts;
}


// =============================================================================
//  Public API
// =============================================================================

bool initRTC() {
  if (!rtc.begin()) {
    Serial.println("[WARN] DS3231 RTC not found!");
    return false;
  }
  if (rtc.lostPower()) {
    Serial.println("[WARN] RTC lost power, setting default time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  DateTime ts = rtc.now();
  Serial.printf("[INFO] RTC: %04d-%02d-%02d %02d:%02d:%02d\n",
                ts.year(), ts.month(), ts.day(),
                ts.hour(), ts.minute(), ts.second());
  return true;
}


void getDateTimeStr(char *buf, size_t len) {
  if (!rtcAvailable) {
    snprintf(buf, len, "%lu", millis() / 1000);
    return;
  }
  DateTime ts = rtcNowSafe();
  snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
           ts.year(), ts.month(), ts.day(),
           ts.hour(), ts.minute(), ts.second());
}

void getTimestampStr(char *buf, size_t len) {
  if (!rtcAvailable) {
    snprintf(buf, len, "%lu", millis());
    return;
  }
  DateTime ts = rtcNowSafe();
  snprintf(buf, len, "%04d%02d%02d_%02d%02d%02d",
           ts.year(), ts.month(), ts.day(),
           ts.hour(), ts.minute(), ts.second());
}

void getDateFolderStr(char *buf, size_t len) {
  if (!rtcAvailable) {
    snprintf(buf, len, "/logs");
    return;
  }
  DateTime ts = rtcNowSafe();
  snprintf(buf, len, "/%04d%02d%02d", ts.year(), ts.month(), ts.day());
}
