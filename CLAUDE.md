# CLAUDE.md — SPL Logger Project Context

> อ่านไฟล์นี้ก่อนเริ่มทำงานทุกครั้ง ให้เข้าใจ context ก่อนตรวจไฟล์

---

## Project Overview

**ชื่อ:** ESP32 SPL + Environmental Logger  
**วัตถุประสงค์:** วัดและบันทึก Sound Pressure Level (SPL) พร้อม GPS + sensor สิ่งแวดล้อม ส่งข้อมูลขึ้น InfluxDB แบบ realtime แสดงผลบน Grafana  
**Branch ปัจจุบัน:** `Iot-Dev`  
**Main directory:** `Iot-ESP32/main/`

---

## Hardware

| ชิ้นส่วน | รุ่น | Interface | GPIO |
|---|---|---|---|
| Microcontroller | ESP32 (Dual-core 240MHz) | — | — |
| Microphone | INMP441 | I2S | SCK=26, WS=25, SD=33 |
| GPS | NEO-8M | UART2 | RX=16, TX=17 |
| Light sensor | BH1750FVI (GY-302) | I2C | SDA=21, SCL=22 |
| UV sensor | GUVA-S12SD | ADC | GPIO 34 |
| Temp/Humidity | DHT22 | 1-Wire | GPIO 23 |
| RTC | DS3231 | I2C | SDA=21, SCL=22 |
| OLED | SH1106 128x64 | I2C | SDA=21, SCL=22 |
| SD Card | SD_MMC 1-bit | SDMMC | DAT0=2, CLK=14, CMD=15 |

---

## Software Architecture

### File Map

```
Iot-ESP32/main/
├── config.h             — ค่า config ทั้งหมด (pins, WiFi, InfluxDB, WDT, backoff, thresholds)
├── globals.h/.cpp       — global variables, Backoff struct, readXxxSafe() (return bool)
├── main.ino             — setup() + loop() + initSDCard() + initQueuesAndMutexes() + WDT init
├── audio_engine.h/.cpp  — I2S driver (install/reinstall), WAV header, micTask (Core 1)
├── sd_logger.h/.cpp     — sdProcessTask, saveSPLValue, SPL compute, WAV record, SD health check
├── sensors.h/.cpp       — luxTask, uvTask, dhtTask, BH1750 init
├── gps_handler.h/.cpp   — gpsTask (UART2 → TinyGPS++)
├── oled_display.h/.cpp  — initOLED, oledShowStatus, oledTask
├── rtc_helper.h/.cpp    — initRTC, getDateTimeStr/getTimestampStr/getDateFolderStr (buffer-based), LOG macro
├── wifi_ntp.h/.cpp      — syncTimeNTP, ensureWiFiConnected (guarded by wifiMutex)
├── cloud_influx.h/.cpp  — influxTask → InfluxDB (HTTP, exponential backoff)
├── tcp_stream.h/.cpp    — tcpStreamTask → EC2 TCP:5001 (write timeout + backoff)
└── cloud_http.h/.cpp    — (ว่าง — WAV upload ถูกลบออกแล้ว)
```

### Conventions / Patterns

ทุก task ใช้ pattern เดียวกัน — ง่ายต่อการเพิ่ม/ลบ task:

```cpp
void someTask(void *parameter) {
  esp_task_wdt_add(NULL);              // 1. ลงทะเบียน WDT
  LOG("TAG", "Task started");          // 2. LOG macro (zero-alloc)

  while (1) {
    esp_task_wdt_reset();              // 3. feed watchdog ทุกรอบ
    // ... work ...
    vTaskDelay(interval / portTICK_PERIOD_MS);
  }
}
```

- **LOG macro** (`rtc_helper.h`): `LOG("TAG", "fmt", ...)` — zero heap allocation, ใช้ buffer-based `getDateTimeStr()`
- **Backoff struct** (`globals.h`): `Backoff b = BACKOFF_DEFAULT(); b.nextMs(); b.reset();`
- **readXxxSafe()** return `bool` — `true` = mutex ได้, `false` = timeout (ข้อมูลเป็น default zero)
- **wifiMutex** ป้องกัน race condition ระหว่าง `influxTask` กับ `tcpStreamTask`

### FreeRTOS Tasks

| Task | Core | Priority | WDT | หน้าที่ |
|---|---|---|---|---|
| `micTask` | 1 | 2 | Yes | อ่าน I2S → fan-out ไป `readyQueue` + `streamReadyQueue` |
| `sdProcessTask` | 0 | 1 | Yes | รับ AudioBlock → WAV + SPL + CSV + SD health check |
| `gpsTask` | 0 | 1 | Yes | parse NMEA จาก UART2 → update gps globals |
| `luxTask` | 0 | 1 | Yes | อ่าน BH1750 ทุก 200ms พร้อม median + EMA filter |
| `uvTask` | 0 | 1 | Yes | อ่าน ADC UV ทุก 500ms (8-sample average) |
| `dhtTask` | 0 | 1 | Yes | อ่าน DHT22 ทุก 2000ms |
| `oledTask` | 0 | 1 | Yes | refresh OLED ทุก 500ms |
| `influxTask` | 0 | 1 | Yes | ส่ง sensor data → InfluxDB ทุก 1000ms (batch 5, backoff) |
| `tcpStreamTask` | 0 | 1 | Yes | stream raw PCM → EC2 TCP:5001 (backoff, write timeout) |

### Data Flow

```
                         ┌─── readyQueue ──► sdProcessTask ──► WAV file (SD)
                         │                                 ──► CSV log (SD)
I2S Mic ──► micTask ─────┤                                 ──► currentSPL
                         │
                         └─── streamReadyQueue ──► tcpStreamTask ──► EC2 TCP:5001

Sensors (BH1750/UV/DHT/GPS) → sensor globals → readXxxSafe() → influxTask → InfluxDB
                                                             → oledTask → OLED
```

### Mutexes

| Mutex | ป้องกัน |
|---|---|
| `sdMutex` | SD_MMC read/write |
| `oledMutex` | display* variables |
| `gpsMutex` | gps* variables |
| `luxMutex` | luxValue, luxValid |
| `uvMutex` | uvIndex, uvValid |
| `dhtMutex` | dhtTemp, dhtHumidity, dhtValid |
| `wireMutex` | I2C bus (BH1750 + RTC + OLED sendBuffer) |
| `wifiMutex` | WiFi.begin() — กัน race ระหว่าง influxTask + tcpStreamTask |

### Stability Features

| Feature | ไฟล์ | รายละเอียด |
|---|---|---|
| **Watchdog Timer** | `main.ino` init, ทุก task | `WDT_TIMEOUT_S` (10s) — panic + reboot ถ้า task ค้าง |
| **I2S Error Recovery** | `audio_engine.cpp` | นับ consecutive errors → `i2s_reinstall()` หลัง threshold |
| **SD Health Check** | `sd_logger.cpp` | นับ SD errors → หยุด recording ถ้าเกิน `SD_ERROR_THRESHOLD` |
| **WiFi Race Guard** | `wifi_ntp.cpp` | `wifiMutex` + double-check pattern |
| **TCP Write Timeout** | `tcp_stream.cpp` | `TCP_WRITE_TIMEOUT_MS` — ป้องกัน task ค้าง |
| **Exponential Backoff** | `tcp_stream.cpp`, `cloud_influx.cpp` | `Backoff` struct — 1.5s → 60s cap, reset on success |
| **Zero-alloc Logging** | `rtc_helper.h` | `LOG()` macro + buffer-based time functions |

### Global Variables (ที่ยังเปิดเป็น global)

| Variable | เหตุผล |
|---|---|
| `currentSPL` | อ่านโดย `influxTask` + `main.ino` loop |
| `isRecording` | อ่านโดย `main.ino` loop (STAT print) |
| `dbFileName` | เขียนใน `main.ino`, อ่านใน `sd_logger.cpp` |
| sensor state vars | เขียนโดย sensor tasks, อ่านผ่าน `readXxxSafe()` |
| display vars | เขียนโดย sensor/sd tasks ผ่าน `oledMutex`, อ่านโดย `oledTask` |

### Variables ที่เป็น static (ไม่เปิด global)

- `sd_logger.cpp`: `audioFile`, `wavFileName`, `lastDbSave`, `splInitialized`, `recordStartTime`, `totalBytesWritten`, `sdErrorCount`, `sdHealthy`
- `computeSPLBlock()`: `rmsAccumulator`, `rmsCount`, `smoothSPL` (static locals)
- `micTask()`: `audioDropCount`, `i2sErrorCount`
- `audio_engine.cpp`: `i2s_install()`, `i2s_reinstall()` เป็น static functions

---

## Infrastructure (AWS — OpenTofu)

**Directory:** `Iot-ESP32/infra/`

| Resource | รายละเอียด |
|---|---|
| EC2 | Ubuntu, Elastic IP: `52.221.18.227` |
| InfluxDB | Docker container, port `8086` |
| Grafana | Docker container, port `3000` |
| S3 Bucket | เก็บ WAV recordings |
| Lambda | (เดิมใช้ presign URL — ยังอยู่ใน infra แต่ไม่ได้ใช้แล้ว) |
| API Gateway | (เดิมใช้กับ Lambda presign — ยังอยู่) |

**Security Group ports เปิดไว้:**
- `22` — SSH
- `8086` — InfluxDB
- `3000` — Grafana
- `5000` — Presign service (เดิม)
- `5001` — TCP audio stream (ESP32 → EC2)

---

## InfluxDB Config

```
URL:    http://52.221.18.227:8086
Org:    c727d2245281d811
Bucket: spl-logger
Device: esp32-spl-001
```

**Fields ที่ส่ง:**
`spl`, `lux`, `lux_ok`, `uv`, `uv_ok`, `temp`, `humi`, `dht_ok`, `lat`, `lng`, `alt`, `speed`, `sats`, `hdop`, `gps_ok`

---

## สิ่งที่ทำเสร็จแล้ว

- [x] Core firmware: mic → SD WAV + CSV logging
- [x] All sensors: GPS, BH1750, GUVA-S12SD, DHT22, DS3231
- [x] InfluxDB realtime push (batch 5, ทุก 1000ms)
- [x] OLED display (5 แถว: time / SPL+rec / lux+uv / temp+humi / gps)
- [x] WiFi reconnect อัตโนมัติ (guarded by wifiMutex)
- [x] NTP sync + RTC update
- [x] TCP Audio Streaming → EC2 TCP:5001 (fan-out from micTask, separate pool)
- [x] Refactor: แยก helper functions, consistent task pattern
- [x] ลบ WAV upload (S3 via Lambda presign) — `cloud_http.h/.cpp` ว่าง
- [x] ลบ unused globals: `uvVoltage`, old `audioDropCount`, SD state vars
- [x] `i2s_install()` + `i2s_reinstall()` เป็น static
- [x] Watchdog Timer ทุก task (10s timeout, panic on hang)
- [x] I2S error recovery (auto re-init after 50 consecutive errors)
- [x] SD card health monitoring (stop recording after 10 consecutive errors)
- [x] TCP write timeout (3s) ป้องกัน task hang
- [x] Exponential backoff สำหรับ InfluxDB + TCP stream (1.5s → 60s)
- [x] Zero-alloc LOG macro + buffer-based time functions (ลด heap fragmentation)
- [x] WiFi reconnect race condition fix (wifiMutex + double-check)
- [x] readXxxSafe() return bool เพื่อแยก "mutex timeout" จาก "sensor invalid"
- [x] Duplicate extern declarations ใน globals.h ลบแล้ว

---

## สิ่งที่วางแผนไว้ (ยังไม่ implement)

### 1. EC2 Side — TCP Audio Server → KVS + S3 WAV

**Architecture:**
```
ESP32 ──raw PCM (TCP port 5001)──► EC2 ──GStreamer kvssink──► Kinesis Video Streams (realtime HLS)
                                       └──ffmpeg pcm_s16le──► S3 (.wav backup)
```

**EC2 side ที่ต้องเพิ่ม:**
- Python TCP server (`/opt/audio_server/server.py`)
- GStreamer + KVS plugin (kvssink)
- KVS stream: `esp32-audio` (ap-southeast-1)

**Listening:**
- Realtime: KVS Live HLS URL → VLC / hls.js (~3-5s lag)
- ย้อนหลัง: KVS On-Demand HLS หรือ S3 `.wav` download

**ffmpeg command สำหรับ WAV backup:**
```bash
ffmpeg -f s16le -ar 16000 -ac 1 -i pipe:0 -codec:a pcm_s16le output.wav
```

### 2. Grafana Dashboard

- เชื่อม InfluxDB data source (localhost:8086)
- Panel: SPL time series, Temp/Humi gauge, UV stat, GPS Geomap
- Auto-refresh 5–10s

---

## Audio Specs

```
Sample rate:  16000 Hz
Bit depth:    16-bit PCM (signed, little-endian)
Channels:     1 (Mono)
Format:       WAV (PCM) on SD, raw PCM over TCP
Record time:  60 วินาที/ไฟล์
File size:    ~1.9 MB/ไฟล์
```

---

## SPL Calibration

```
MIC_OFFSET_DB = 115.0f   (INMP441 calibration offset)
RMS_WINDOW    = 30 blocks (accumulate ก่อน compute)
EMA_ALPHA     = 0.3f      (smoothing)
```

---

## IDE Notes

VSCode + clangd report false-positive errors (`Arduino.h not found`, `Serial`, `portTICK_PERIOD_MS` ฯลฯ) เพราะ clangd ไม่รู้จัก ESP32 Arduino SDK path — **ไม่ใช่ error จริง** compile ผ่านปกติใน Arduino IDE
