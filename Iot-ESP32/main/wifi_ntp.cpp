#include "wifi_ntp.h"
#include "globals.h"
#include "rtc_helper.h"

// ========== WiFi NTP Sync ==========
// Connect WiFi -> sync NTP -> set RTC -> disconnect
void syncTimeNTP() {
  Serial.printf("[%s] [INFO] Connecting WiFi '%s'...\n",
                getDateTimeString().c_str(), WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED &&
         (millis() - startAttempt) < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%s] [WARN] WiFi not available, skipping NTP sync.\n",
                  getDateTimeString().c_str());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  Serial.printf("[%s] [INFO] WiFi connected! IP: %s\n",
                getDateTimeString().c_str(), WiFi.localIP().toString().c_str());

  configTime(NTP_GMT_OFFSET, NTP_DAYLIGHT_OFFSET, NTP_SERVER);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    Serial.printf("[%s] [INFO] NTP synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                  getDateTimeString().c_str(), timeinfo.tm_year + 1900,
                  timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
                  timeinfo.tm_min, timeinfo.tm_sec);

    if (rtcAvailable) {
      rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                          timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
                          timeinfo.tm_sec));
      Serial.printf("[%s] [INFO] RTC updated from NTP.\n",
                    getDateTimeString().c_str());
    }
  } else {
    Serial.printf("[%s] [WARN] NTP sync failed, using existing RTC time.\n",
                  getDateTimeString().c_str());
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("[%s] [INFO] WiFi disconnected (power save).\n",
                getDateTimeString().c_str());
}
