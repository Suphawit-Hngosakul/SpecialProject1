#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <Arduino.h>

void initOLED();
void oledTask(void *parameter);


void oledShowStatus(const char *step, const char *detail = nullptr, const char *extra = nullptr);

#endif // OLED_DISPLAY_H
