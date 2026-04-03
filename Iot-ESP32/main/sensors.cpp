#include "sensors.h"
#include "globals.h"
#include "rtc_helper.h"

// ========== BH1750 Init ==========
bool initBH1750() {
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("[WARN] BH1750 not found! Check wiring & address.");
    return false;
  }
  Serial.println("[INFO] BH1750 initialized.");
  return true;
}

// ========== UV Sensor Init ==========
void initUV() {
  pinMode(UV_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  Serial.printf("[INFO] GUVA-S12SD UV sensor initialized (GPIO %d).\n", UV_PIN);
}

// ========== DHT22 Init ==========
void initDHT() {
  dht.begin();
  Serial.printf("[INFO] DHT22 initialized (GPIO %d).\n", DHT_PIN);
}

// ========== UV Voltage -> UV Index (Lookup Table) ==========
float voltageToUVIndex(float voltageMV) {
  const float table[][2] = {{50, 0},  {227, 1}, {318, 2},   {408, 3},
                            {503, 4}, {590, 5}, {658, 6},   {770, 7},
                            {881, 8}, {976, 9}, {1079, 10}, {1170, 11}};
  const int n = sizeof(table) / sizeof(table[0]);

  if (voltageMV <= table[0][0])
    return 0.0f;
  if (voltageMV >= table[n - 1][0])
    return table[n - 1][1];

  for (int i = 0; i < n - 1; i++) {
    if (voltageMV < table[i + 1][0]) {
      float ratio = (voltageMV - table[i][0]) / (table[i + 1][0] - table[i][0]);
      return table[i][1] + ratio * (table[i + 1][1] - table[i][1]);
    }
  }
  return 11.0f;
}

// ========== Light Sensor Task (Core 0) ==========
// ใช้ CONTINUOUS mode เหมือน test code ที่ทำงานได้นิ่ง
// sensor วัดเองตลอด ไม่ต้อง trigger → wireMutex ถูกยึดสั้นมาก (แค่ตอนอ่าน)
void luxTask(void *parameter) {
  Serial.printf("[%s] [INFO] Lux Task started (slope=%.3f offset=%.1f)\n",
                getDateTimeString().c_str(), LUX_CAL_SLOPE, LUX_CAL_OFFSET);

  const float LUX_EMA_ALPHA = 0.15f;
  const float LUX_DEADBAND  = 3.0f;
  float emaLux = 0.0f;
  bool  emaInit = false;

  while (1) {
    // เช็ค measurementReady — เหมือน test code
    bool ready = false;
    if (xSemaphoreTake(wireMutex, 150 / portTICK_PERIOD_MS)) {
      ready = lightMeter.measurementReady();
      xSemaphoreGive(wireMutex);
    }

    if (ready) {
      float rawLux = -1.0f;
      if (xSemaphoreTake(wireMutex, 150 / portTICK_PERIOD_MS)) {
        rawLux = lightMeter.readLightLevel();
        xSemaphoreGive(wireMutex);
      }

      bool ok = (rawLux >= 0.0f);
      if (ok) {
        // 2-point linear calibration
        float calibrated = rawLux * LUX_CAL_SLOPE + LUX_CAL_OFFSET;
        if (calibrated < 0.0f) calibrated = 0.0f;

        if (!emaInit) {
          emaLux = calibrated;
          emaInit = true;
        } else {
          emaLux = LUX_EMA_ALPHA * calibrated + (1.0f - LUX_EMA_ALPHA) * emaLux;
        }

        if (xSemaphoreTake(luxMutex, 20 / portTICK_PERIOD_MS)) {
          if (!luxValid || fabsf(emaLux - luxValue) >= LUX_DEADBAND)
            luxValue = emaLux;
          luxValid = true;
          xSemaphoreGive(luxMutex);
        }
      } else {
        if (xSemaphoreTake(luxMutex, 20 / portTICK_PERIOD_MS)) {
          luxValid = false;
          xSemaphoreGive(luxMutex);
        }
      }

      if (xSemaphoreTake(oledMutex, 10 / portTICK_PERIOD_MS)) {
        displayLux = luxValue;
        displayLuxValid = luxValid;
        xSemaphoreGive(oledMutex);
      }
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ========== UV Sensor Task (Core 0) ==========
void uvTask(void *parameter) {
  Serial.printf(
      "[%s] [INFO] UV Task started (GPIO %d) [Calibrated ADC + Lookup Table]\n",
      getDateTimeString().c_str(), UV_PIN);

  while (1) {
    uint32_t mvSum = 0;
    for (int i = 0; i < 8; i++) {
      mvSum += analogReadMilliVolts(UV_PIN);
      vTaskDelay(5 / portTICK_PERIOD_MS);
    }
    float voltageMV = mvSum / 8.0f;
    float volt = voltageMV / 1000.0f;
    float uvIdx = voltageToUVIndex(voltageMV);
    if (uvIdx < 0.0f)
      uvIdx = 0.0f;

    if (xSemaphoreTake(uvMutex, 20 / portTICK_PERIOD_MS)) {
      uvVoltage = volt;
      uvIndex = uvIdx;
      uvValid = true;
      xSemaphoreGive(uvMutex);
    }

    if (xSemaphoreTake(oledMutex, 10 / portTICK_PERIOD_MS)) {
      displayUVIndex = uvIdx;
      displayUVValid = true;
      xSemaphoreGive(oledMutex);
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ========== DHT22 Task (Core 0) ==========
void dhtTask(void *parameter) {
  Serial.printf("[%s] [INFO] DHT22 Task started (GPIO %d)\n",
                getDateTimeString().c_str(), DHT_PIN);

  vTaskDelay(3000 / portTICK_PERIOD_MS);

  while (1) {
    float temp = dht.readTemperature();
    float humi = dht.readHumidity();
    bool ok = (!isnan(temp) && !isnan(humi));
    float corrTemp = temp + DHT_TEMP_OFFSET;
    float corrHumi = humi + DHT_HUMID_OFFSET;
    corrHumi = constrain(corrHumi, 0.0f, 100.0f);

    if (xSemaphoreTake(dhtMutex, 20 / portTICK_PERIOD_MS)) {
      dhtTemp = ok ? corrTemp : dhtTemp;
      dhtHumidity = ok ? corrHumi : dhtHumidity;
      dhtValid = ok;
      xSemaphoreGive(dhtMutex);
    }

    if (xSemaphoreTake(oledMutex, 10 / portTICK_PERIOD_MS)) {
      displayTemp = dhtTemp;
      displayHumidity = dhtHumidity;
      displayDHTValid = dhtValid;
      xSemaphoreGive(oledMutex);
    }

    if (!ok) {
      Serial.printf("[%s] [WARN] DHT22 read failed!\n",
                    getDateTimeString().c_str());
    }

    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}
