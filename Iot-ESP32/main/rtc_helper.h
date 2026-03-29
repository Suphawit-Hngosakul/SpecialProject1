#ifndef RTC_HELPER_H
#define RTC_HELPER_H

#include <Arduino.h>

bool initRTC();
String getTimestamp();
String getDateTimeString();
String getDateFolder();

#endif // RTC_HELPER_H
