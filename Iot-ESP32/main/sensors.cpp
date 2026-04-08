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

// ========== Median Filter Helper ==========
// Insertion-sort based median — O(n²) แต่ n=3 ไม่เป็นปัญหา
static float medianFilter(float *buf, int n) {
  if (n > LUX_MEDIAN_WINDOW) n = LUX_MEDIAN_WINDOW; // safety clamp: ป้องกัน stack overflow
  float sorted[LUX_MEDIAN_WINDOW];
  memcpy(sorted, buf, n * sizeof(float));
  for (int i = 1; i < n; i++) {
    float key = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
  }
  return sorted[n / 2];
}

// ========== Light Sensor Task (Core 0) ==========
// FIX: รวม measurementReady() + readLightLevel() ใน lock เดียว
//      + Median Filter ตัด spike จาก I2C bus contention
void luxTask(void *parameter) {
  Serial.printf("[%s] [INFO] Lux Task started (slope=%.3f offset=%.1f median=%d)\n",
                getDateTimeString().c_str(), LUX_CAL_SLOPE, LUX_CAL_OFFSET, LUX_MEDIAN_WINDOW);

  const float LUX_EMA_ALPHA_LOCAL = LUX_EMA_ALPHA; // ใช้ค่าจาก config.h
  const float LUX_DEADBAND  = 1.0f;   // 3.0→1.0: ปล่อยการเปลี่ยนเล็กๆ ผ่าน
  float emaLux = 0.0f;
  bool  emaInit = false;

  // Median filter circular buffer
  float medianBuf[LUX_MEDIAN_WINDOW] = {0};
  int   medianIdx = 0;
  int   medianCount = 0;

  int failCount = 0;
  const int FAIL_REINIT_THRESHOLD = 10; // re-init หลัง fail ต่อเนื่อง 10 ครั้ง

  while (1) {
    // ===== FIX: lock wireMutex ครั้งเดียว สำหรับทั้ง ready check + read =====
    // ป้องกัน OLED sendBuffer() แทรกระหว่าง 2 operations
    float rawLux = -1.0f;
    bool  readOk = false;

    if (xSemaphoreTake(wireMutex, 200 / portTICK_PERIOD_MS)) {
      if (lightMeter.measurementReady()) {
        rawLux = lightMeter.readLightLevel();
        readOk = (rawLux >= 0.0f);
      }
      xSemaphoreGive(wireMutex);
    }

    if (readOk) {
      failCount = 0;

      // 2-point linear calibration
      float calibrated = rawLux * LUX_CAL_SLOPE + LUX_CAL_OFFSET;
      if (calibrated < 0.0f) calibrated = 0.0f;

      // ===== Median Filter: ตัด spike ที่เกิดจาก I2C glitch =====
      medianBuf[medianIdx] = calibrated;
      medianIdx = (medianIdx + 1) % LUX_MEDIAN_WINDOW;
      if (medianCount < LUX_MEDIAN_WINDOW) medianCount++;

      float filtered = (medianCount >= LUX_MEDIAN_WINDOW)
                          ? medianFilter(medianBuf, LUX_MEDIAN_WINDOW)
                          : calibrated; // ยังไม่ครบ window → ใช้ค่าตรง

      // ===== EMA Smoothing (on top of median) =====
      if (!emaInit) {
        emaLux = filtered;
        emaInit = true;
      } else {
        emaLux = LUX_EMA_ALPHA_LOCAL * filtered + (1.0f - LUX_EMA_ALPHA_LOCAL) * emaLux;
      }

      if (xSemaphoreTake(luxMutex, 20 / portTICK_PERIOD_MS)) {
        if (!luxValid || fabsf(emaLux - luxValue) >= LUX_DEADBAND)
          luxValue = emaLux;
        luxValid = true;
        xSemaphoreGive(luxMutex);
      }
    } else {
      failCount++;

      // Re-init BH1750 หลัง fail ต่อเนื่อง (bus อาจ hang)
      if (failCount >= FAIL_REINIT_THRESHOLD) {
        Serial.printf("[%s] [WARN] BH1750 %d consecutive fails — re-initializing\n",
                      getDateTimeString().c_str(), failCount);
        if (xSemaphoreTake(wireMutex, 200 / portTICK_PERIOD_MS)) {
          lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
          xSemaphoreGive(wireMutex);
        }
        failCount = 0;
        vTaskDelay(200 / portTICK_PERIOD_MS); // ให้ sensor settle
      }

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

    if (xSemaphoreTake(dhtMutex, 20 / portTICK_PERIOD_MS)) {
      if (ok) {
        // คำนวณ correction เฉพาะเมื่อค่า valid — ป้องกัน NaN propagation
        dhtTemp     = temp + DHT_TEMP_OFFSET;
        dhtHumidity = constrain(humi + DHT_HUMID_OFFSET, 0.0f, 100.0f);
      }
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
