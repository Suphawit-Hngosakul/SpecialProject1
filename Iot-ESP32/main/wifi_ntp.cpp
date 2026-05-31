#include "wifi_ntp.h"
#include "globals.h"
#include "rtc_helper.h"

#include <esp_task_wdt.h>


// =============================================================================
//  Try a single SSID/PASS — blocks up to WIFI_TIMEOUT_MS. Returns true on success.
// =============================================================================

static bool tryConnect(const char *ssid, const char *pass, bool useSerialDots) {
  LOG("WIFI", "Trying '%s'...", ssid);

  WiFi.disconnect(false, true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - start) < WIFI_TIMEOUT_MS) {
    esp_task_wdt_reset();   // no-op if caller task isn't WDT-registered
    if (useSerialDots) Serial.print(".");
    vTaskDelay(250 / portTICK_PERIOD_MS);
  }
  if (useSerialDots) Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    LOG("WIFI", "Connected to '%s'. IP: %s", ssid,
        WiFi.localIP().toString().c_str());
    return true;
  }
  LOG("WIFI", "Failed to connect '%s'.", ssid);
  return false;
}


// =============================================================================
//  Initial NTP Sync (called once from setup — before tasks start)
//
//  Returns true when WiFi is connected (regardless of NTP outcome).
//  Returns false only when both SSIDs fail → caller enters OFFLINE mode.
// =============================================================================

bool syncTimeNTP() {
  WiFi.mode(WIFI_STA);

  bool connected = tryConnect(WIFI_SSID_1, WIFI_PASS_1, true) ||
                   tryConnect(WIFI_SSID_2, WIFI_PASS_2, true);

  if (!connected) {
    LOG("WARN", "No WiFi available — entering OFFLINE mode.");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  // Multiple servers — first one that responds wins
  configTime(NTP_GMT_OFFSET, NTP_DAYLIGHT_OFFSET,
             NTP_SERVER, "time.google.com", "time.nist.gov");

  // Retry up to 3 × 5s — handles slow networks / first-DNS-resolve delay
  struct tm timeinfo;
  bool ntpOk = false;
  for (int attempt = 1; attempt <= 3 && !ntpOk; attempt++) {
    LOG("NTP", "Sync attempt %d/3...", attempt);
    if (getLocalTime(&timeinfo, 5000)) {
      ntpOk = true;
      break;
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }

  if (ntpOk) {
    LOG("INFO", "NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    if (rtcAvailable) {
      rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                          timeinfo.tm_mday, timeinfo.tm_hour,
                          timeinfo.tm_min, timeinfo.tm_sec));
      LOG("INFO", "RTC updated from NTP.");
    }
  } else {
    LOG("WARN", "NTP sync failed after 3 attempts — using existing RTC time.");
  }

  LOG("INFO", "WiFi remains connected for cloud tasks.");
  return true;
}


// =============================================================================
//  WiFi Reconnect  (thread-safe — guarded by wifiMutex)
// =============================================================================

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;

  // Only one task may attempt reconnection at a time
  MutexLock lock(wifiMutex, 50);
  if (!lock) {
    // Another task is already reconnecting — wait and re-check
    vTaskDelay(500 / portTICK_PERIOD_MS);
    return WiFi.status() == WL_CONNECTED;
  }

  // Double-check after acquiring mutex (another task may have reconnected)
  if (WiFi.status() == WL_CONNECTED) return true;

  bool ok = tryConnect(WIFI_SSID_1, WIFI_PASS_1, false) ||
            tryConnect(WIFI_SSID_2, WIFI_PASS_2, false);

  if (!ok) LOG("WIFI", "All SSIDs failed.");
  return ok;
}
