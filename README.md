# Mateo — Community Weather Network

Distributed open-source weather station network. People build stations at home, data flows to a central server, apps consume it.

---

## System Overview

```
[Weather Station] --433MHz ASK--> [Home Node] --REST API--> [Server] --API--> [Mobile App]
```

The system has two physical builds and a backend:

- **Weather Station** — outdoor unit, battery/solar powered, reads sensors, transmits over 433 MHz RF
- **Home Node** — indoor unit, mains powered, receives RF from the station, shows data on a local display, forwards to the server over Wi-Fi
- **Server** — receives data from nodes, serves it to mobile apps and web page

---

## Weather Station

### What it does

Runs on battery. Spends most of its time in deep sleep. Wakes up, reads all sensors, compares values to the last transmission, and decides how long to sleep next — between 5 and 30 minutes depending on how much the readings changed. Encodes the data into a compact ASK packet and sends it over 433 MHz RF to the node.

### Hardware

| Component | Part | Link |
|-----------|------|------|
| MCU | ESP32 DevKitC 38-pin | [dratek.cz](https://dratek.cz/arduino-platforma/51547-esp32-devkitc-development-board-38pin.html) |
| Temp / Humidity / Pressure | BME280 (I²C) | [dratek.cz](https://dratek.cz/arduino-platforma/1361-bme280-modul-mereni-teploty-vlhkosti-a-barometrickeho-tlaku-precizni.html) |
| Air quality | MQ-2 (analog out) | [dratek.cz](https://dratek.cz/arduino-platforma/1074-mq2-mq-2-senzor-horlavych-plynu-propanu-metanu-butanu-vodiku.html) |
| RF transmitter | NiceRF STX882 433 MHz ASK | [dratek.cz](https://dratek.cz/arduino-platforma/3172-nicerf-433mhz-vysilac-prijimac-2x-antena-set-4ks.html) |
| Rain | Tipping bucket rain gauge (reed switch) | — |
| Power | LiPo + solar charge controller | — |

### Wiring

| Signal | GPIO | Notes |
|--------|------|-------|
| BME280 SDA | 21 | I²C, 3.3 V |
| BME280 SCL | 22 | I²C, 3.3 V |
| MQ-2 AOUT | 34 | ADC1, MQ-2 VCC at 5 V |
| STX882 DATA | 4 | 3.3 V, CS floating = always active |
| Rain gauge | 27 | Reed switch to GND, internal pull-up, interrupt-driven tip counter |

### Firmware logic

1. Wake from deep sleep
2. Read BME280 (temp, humidity, pressure) + MQ-2 (analog) + rain tip counter from RTC memory
3. Diff against last transmitted values
4. Set next sleep duration: large delta → 5 min, stable readings → up to 30 min
5. Encode packet and transmit via STX882
6. Store current values in RTC memory
7. Deep sleep

---

## Home Node

### What it does

Runs continuously indoors. Listens for RF packets from the station, renders readings on the TFT display, and forwards data to the server via REST. If the server is unreachable, saves packets to SD card with timestamps and retries every 5 minutes. Once connection is restored, flushes the SD card in chronological order and wipes it.

### Hardware

| Component | Part | Link |
|-----------|------|------|
| MCU | ESP32 DevKitC 38-pin | [dratek.cz](https://dratek.cz/arduino-platforma/51547-esp32-devkitc-development-board-38pin.html) |
| RF receiver | NiceRF SRX887 433 MHz | [dratek.cz](https://dratek.cz/arduino-platforma/3172-nicerf-433mhz-vysilac-prijimac-2x-antena-set-4ks.html) |
| Display | ILI9488 3.5" 480×320 SPI TFT | [dratek.cz](https://dratek.cz/arduino-platforma/149154-dotykovy-displej-3-5-480x320-spi-tft-ili9488.html) |
| Offline storage | MicroSD module (SPI) | — |
| Power | 5 V USB-C | — |

### Wiring

| Signal | GPIO | Notes |
|--------|------|-------|
| SRX887 DATA | 15 | 3.3 V |
| SRX887 CS | 2 | Active LOW — pull HIGH to sleep |
| TFT CS | 5 | VSPI |
| TFT DC | 2 | — |
| TFT RESET | 4 | — |
| MOSI (shared) | 23 | TFT + SD share VSPI bus |
| SCK (shared) | 18 | — |
| MISO | 19 | SD only |
| SD CS | 13 | — |

### Firmware logic

1. Receive RF packet from station
2. Decode and update TFT display
3. Check server connectivity (`GET /api/ping`)
4. **Online:** `POST /api/readings` immediately
5. **Offline:** append packet + ISO 8601 timestamp to SD card
6. Retry ping every 5 minutes while offline
7. On reconnect: flush SD records to server in order → wipe SD

---

## Server

Hosted at `api.eggmanstudio.me`. Two API roots — one for devices, one for apps. Web dashboard and station registration at `weather.eggmanstudio.me`.

### Device API — `https://api.eggmanstudio.me/api`

Used by the home node.

**POST** `/readings`
```json
{
  "station_id": "abc123",
  "timestamp": "2026-06-18T14:30:00Z",
  "temperature_c": 21.4,
  "humidity_pct": 58.2,
  "pressure_hpa": 1013.1,
  "air_quality_raw": 312,
  "rainfall_mm": 0.5
}
```

**GET** `/ping` — returns `{ "status": "ok" }`, used by node to check connectivity before flushing SD

### App API — `https://api.eggmanstudio.me/appapi`

Used by mobile apps.

**GET** `/stations/{station_id}/latest`

**GET** `/stations/{station_id}/readings?from=2026-06-01&to=2026-06-18`

All endpoints require `Authorization: Bearer <api_key>`.

---

## Firmware Setup

Arduino framework, ESP32 core 2.x.

**Libraries:**
- `Adafruit BME280` + `Adafruit Unified Sensor`
- `TFT_eSPI` (configure `User_Setup.h` for ILI9488 on VSPI)
- `RadioHead` (`RH_ASK` driver)
- `ArduinoJson`
- `SD`



**Station config** — `firmware/station/config.h`
```cpp
#define STATION_ID          "your-station-id" //you will be given an id after sigining up
#define TX_PIN              4
#define RAIN_PIN            27
#define SEND_INTERVAL_MIN   5    // minutes
#define SEND_INTERVAL_MAX   30   // minutes
```

**Node config** — `firmware/node/config.h`
```cpp
#define WIFI_SSID     "ssid"
#define WIFI_PASS     "password"
#define API_KEY       "your-api-key" //you will be given an api key after signing up
#define SERVER_URL    "https://api.eggmanstudio.me/api"
#define RX_PIN        15
```

---

## Self-Hosting

```bash
cd Mateo/server
cp .env.example .env   # set DB credentials and secret key
docker-compose up -d
```

Node.js 18+ if running without Docker.

---

## Contributing

PRs welcome — firmware, sensor drivers, PCB layouts, server, apps.

```bash
git checkout -b feature/your-thing
```