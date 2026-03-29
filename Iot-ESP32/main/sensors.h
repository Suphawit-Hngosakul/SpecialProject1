#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>

bool initBH1750();
void initUV();
void initDHT();

float voltageToUVIndex(float voltageMV);

void luxTask(void *parameter);
void uvTask(void *parameter);
void dhtTask(void *parameter);

#endif // SENSORS_H
