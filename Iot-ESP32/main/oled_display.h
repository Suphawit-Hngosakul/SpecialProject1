#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <Arduino.h>

void initOLED();
void oledTask(void *parameter);

// แสดงสถานะ setup บน OLED (ใช้ระหว่าง setup() ก่อน task start)
// step   = หัวข้อหลัก (บรรทัดใหญ่)
// detail = รายละเอียด (optional)
// extra  = ข้อมูลเพิ่มเติม เช่น countdown (optional)
void oledShowStatus(const char *step, const char *detail = nullptr, const char *extra = nullptr);

#endif // OLED_DISPLAY_H
