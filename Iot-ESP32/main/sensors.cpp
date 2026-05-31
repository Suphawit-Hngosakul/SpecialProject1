#include "sensors.h"
#include "globals.h"
#include "rtc_helper.h"

#include <esp_task_wdt.h>


// =============================================================================
//  Sensor Init
// =============================================================================

bool initBH1750() {
  const int MAX_ATTEMPTS = 3;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
      if (attempt > 1)
        Serial.printf("[INFO] BH1750 initialized (attempt %d/%d).\n", attempt, MAX_ATTEMPTS);
      else
        Serial.println("[INFO] BH1750 initialized.");
      return true;
    }
    Serial.printf("[WARN] BH1750 init attempt %d/%d failed, retrying...\n", attempt, MAX_ATTEMPTS);
    delay(200);
  }
  Serial.println("[WARN] BH1750 not found! Check wiring & address.");
  return false;
}

void initUV() {
  pinMode(UV_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  Serial.printf("[INFO] GUVA-S12SD UV sensor initialized (GPIO %d).\n", UV_PIN);
}

void initDHT() {
  dht.begin();
  Serial.printf("[INFO] DHT22 initialized (GPIO %d).\n", DHT_PIN);
}


// =============================================================================
//  UV Voltage → Index  (lookup table + linear interpolation)
// =============================================================================

float voltageToUVIndex(float voltageMV) {
  static const float table[][2] = {
      {50, 0},  {227, 1}, {318, 2},   {408, 3},
      {503, 4}, {590, 5}, {658, 6},   {770, 7},
      {881, 8}, {976, 9}, {1079, 10}, {1170, 11}};
  static const int n = sizeof(table) / sizeof(table[0]);

  if (voltageMV <= table[0][0])     return 0.0f;
  if (voltageMV >= table[n - 1][0]) return table[n - 1][1];

  for (int i = 0; i < n - 1; i++) {
    if (voltageMV < table[i + 1][0]) {
      float ratio = (voltageMV - table[i][0]) / (table[i + 1][0] - table[i][0]);
      return table[i][1] + ratio * (table[i + 1][1] - table[i][1]);
    }
  }
  return 11.0f;
}


// =============================================================================
//  Median Filter  (insertion-sort, window = LUX_MEDIAN_WINDOW)
// =============================================================================

static float medianFilter(float *buf, int n) {
  if (n > LUX_MEDIAN_WINDOW) n = LUX_MEDIAN_WINDOW;
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


// =============================================================================
//  luxTask  (Core 0) — BH1750, 200 ms cycle, median + EMA
// =============================================================================

void luxTask(void *parameter) {
  esp_task_wdt_add(NULL);
  LOG("INFO", "Lux Task started (slope=%.3f offset=%.1f median=%d)",
      LUX_CAL_SLOPE, LUX_CAL_OFFSET, LUX_MEDIAN_WINDOW);

  float emaLux  = 0.0f;
  bool  emaInit = false;
  float medianBuf[LUX_MEDIAN_WINDOW] = {0};
  int   medianIdx   = 0;
  int   medianCount = 0;
  int   failCount   = 0;
  const int FAIL_REINIT_THRESHOLD = 10;

  while (1) {
    esp_task_wdt_reset();

    float rawLux = -1.0f;
    bool  readOk = false;

    {
      MutexLock l(wireMutex, 200);
      if (l && lightMeter.measurementReady()) {
        rawLux = lightMeter.readLightLevel();
        readOk = (rawLux >= 0.0f);
      }
    }

    if (readOk) {
      failCount = 0;

      float calibrated = rawLux * LUX_CAL_SLOPE + LUX_CAL_OFFSET;
      if (calibrated < 0.0f) calibrated = 0.0f;

      // Median filter
      medianBuf[medianIdx] = calibrated;
      medianIdx = (medianIdx + 1) % LUX_MEDIAN_WINDOW;
      if (medianCount < LUX_MEDIAN_WINDOW) medianCount++;

      float filtered = (medianCount >= LUX_MEDIAN_WINDOW)
                            ? medianFilter(medianBuf, LUX_MEDIAN_WINDOW)
                            : calibrated;

      // EMA smoothing
      if (!emaInit) { emaLux = filtered; emaInit = true; }
      else          { emaLux = LUX_EMA_ALPHA * filtered + (1.0f - LUX_EMA_ALPHA) * emaLux; }

      MutexLock l(luxMutex, 20);
      if (l) {
        const float LUX_DEADBAND = 1.0f;
        if (!luxValid || fabsf(emaLux - luxValue) >= LUX_DEADBAND)
          luxValue = emaLux;
        luxValid = true;
      }
    } else {
      failCount++;
      if (failCount >= FAIL_REINIT_THRESHOLD) {
        LOG("WARN", "BH1750 %d consecutive fails — re-initializing", failCount);
        {
          MutexLock l(wireMutex, 200);
          if (l) lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
        }
        failCount = 0;
        vTaskDelay(200 / portTICK_PERIOD_MS);
      }

      MutexLock l(luxMutex, 20);
      if (l) luxValid = false;
    }

    {
      MutexLock l(oledMutex, 10);
      if (l) {
        displayLux      = luxValue;
        displayLuxValid = luxValid;
      }
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}


// =============================================================================
//  uvTask  (Core 0) — GUVA-S12SD ADC, 500 ms cycle, 8-sample average
// =============================================================================

void uvTask(void *parameter) {
  esp_task_wdt_add(NULL);
  LOG("INFO", "UV Task started (GPIO %d) [Calibrated ADC + Lookup Table]", UV_PIN);

  while (1) {
    esp_task_wdt_reset();

    uint32_t mvSum = 0;
    for (int i = 0; i < 8; i++) {
      mvSum += analogReadMilliVolts(UV_PIN);
      vTaskDelay(5 / portTICK_PERIOD_MS);
    }
    float voltageMV = mvSum / 8.0f;
    float uvIdx = voltageToUVIndex(voltageMV);
    if (uvIdx < 0.0f) uvIdx = 0.0f;

    {
      MutexLock l(uvMutex, 20);
      if (l) { uvIndex = uvIdx; uvValid = true; }
    }
    {
      MutexLock l(oledMutex, 10);
      if (l) { displayUVIndex = uvIdx; displayUVValid = true; }
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}


// =============================================================================
//  dhtTask  (Core 0) — DHT22, 2000 ms cycle
// =============================================================================

void dhtTask(void *parameter) {
  esp_task_wdt_add(NULL);
  LOG("INFO", "DHT22 Task started (GPIO %d)", DHT_PIN);

  vTaskDelay(500 / portTICK_PERIOD_MS);

  while (1) {
    esp_task_wdt_reset();

    float temp = dht.readTemperature();
    float humi = dht.readHumidity();
    bool ok = (!isnan(temp) && !isnan(humi));

    {
      MutexLock l(dhtMutex, 20);
      if (l) {
        if (ok) {
          dhtTemp     = temp + DHT_TEMP_OFFSET;
          dhtHumidity = constrain(humi + DHT_HUMID_OFFSET, 0.0f, 100.0f);
        }
        dhtValid = ok;
      }
    }
    {
      MutexLock l(oledMutex, 10);
      if (l) {
        displayTemp     = dhtTemp;
        displayHumidity = dhtHumidity;
        displayDHTValid = dhtValid;
      }
    }

    if (!ok) LOG("WARN", "DHT22 read failed!");

    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}
