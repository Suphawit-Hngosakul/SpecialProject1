#ifndef RTC_HELPER_H
#define RTC_HELPER_H

#include <Arduino.h>

// =============================================================================
//  RTC / Time Helpers
// =============================================================================

bool initRTC();

// Buffer sizes for time strings
#define DATETIME_BUF_SIZE  24
#define TIMESTAMP_BUF_SIZE 20
#define DATEFOLDER_BUF_SIZE 12

// Zero-alloc time string functions — write into caller-provided buffer
void getDateTimeStr(char *buf, size_t len);    // "2025-04-17 14:30:00"
void getTimestampStr(char *buf, size_t len);   // "20250417_143000"
void getDateFolderStr(char *buf, size_t len);  // "/20250417"


// =============================================================================
//  LOG Macro  — zero-alloc Serial.printf with timestamp
//
//  Usage:  LOG("INFO", "value=%d", 42);
//  Output: [2025-04-17 14:30:00] [INFO] value=42
// =============================================================================

#define LOG(tag, fmt, ...) do { \
    char _dt[DATETIME_BUF_SIZE]; \
    getDateTimeStr(_dt, sizeof(_dt)); \
    Serial.printf("[%s] [" tag "] " fmt "\n", _dt, ##__VA_ARGS__); \
} while (0)

#endif // RTC_HELPER_H
