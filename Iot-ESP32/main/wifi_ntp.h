#ifndef WIFI_NTP_H
#define WIFI_NTP_H

#include <Arduino.h>

// Connect WiFi (tries SSID_1 then SSID_2) and sync RTC from NTP.
// Returns true when WiFi is connected — NTP success/failure doesn't change return.
// Returns false when both SSIDs fail → caller should enter OFFLINE mode.
bool syncTimeNTP();

// Reconnect WiFi if disconnected. Returns true when connected.
// Safe to call from any task (tries WIFI_SSID_1 then WIFI_SSID_2 from config.h).
bool ensureWiFiConnected();

#endif // WIFI_NTP_H
