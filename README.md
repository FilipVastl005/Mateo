# Mateo — Community Weather Network

Open-source distributed weather station network. Build a station, contribute data.

```
[Station: ESP32 + sensors] --ASK 433MHz--> [Node: ESP32 + TFT] --REST--> [api.eggmanstudio.me] --> [App]
```

**Station** — battery/solar powered, deep sleep between readings, transmits over 433 MHz RF.  
**Node** — mains powered, receives RF, drives local display, forwards to API. Falls back to SD card when offline, syncs on reconnect.

---

## Hardware

### Station

| Component | Part | Link |
|-----------|------|------|
| MCU | ESP32 DevKitC 38-pin | [dratek.cz](https://dratek.cz/arduino-platforma/51547-esp32-devkitc-development-board-38pin.html) |
| Temp / Humidity / Pressure | BME280 (I²C) | [dratek.cz](https://dratek.cz/arduino-platforma/1361-bme280-modul-mereni-teploty-vlhkosti-a-barometrickeho-tlaku-precizni.html) |
| Air quality | MQ-2 (analog) | [dratek.cz](https://dratek.cz/arduino-platforma/1074-mq2-mq-2-senzor-horlavych-plynu-propanu-metanu-butanu-vodiku.html) |
| RF TX | NiceRF STX882 433 MHz ASK | [dratek.cz](https://dratek.cz/arduino-platforma/3172-nicerf-433mhz-vysilac-prijimac-2x-antena-set-4ks.html) |
| Rain | Tipping bucket (reed switch → GPIO interrupt) | — |
| Power | LiPo + solar charge controller | — |

### Node

| Component | Part | Link |
|-----------|------|------|
| MCU | ESP32 DevKitC 38-pin | [dratek.cz](https://dratek.cz/arduino-platforma/51547-esp32-devkitc-development-board-38pin.html) |
| RF RX | NiceRF SRX887 433 MHz | [dratek.cz](https://dratek.cz/arduino-platforma/3172-nicerf-433mhz-vysilac-prijimac-2x-antena-set-4ks.html) |
| Display | ILI9488 3.5" 480×320 SPI TFT | [dratek.cz](https://dratek.cz/arduino-platforma/149154-dotykovy-displej-3-5-480x320-spi-tft-ili9488.html) |
| Storage | MicroSD module (SPI) | — |
| Power | 5V USB-C | — |

---

## Pin Mapping

### Station

| Signal | GPIO |
|--------|------|
| BME280 SDA (I²C) | 21 |
| BME280 SCL (I²C) | 22 |
| MQ-2 AOUT (ADC1) | 34 |
| STX882 DATA | 4 |
| Rain gauge (interrupt) | 27 |

BME280 at 3.3 V. MQ-2 VCC at 5 V, AOUT tolerates 3.3 V logic. STX882 VCC at 3.3 V, CS left floating (always active).

### Node

| Signal | GPIO |
|--------|------|
| SRX887 DATA | 15 |
| SRX887 CS (active LOW) | 2 |
| TFT CS | 5 |
| TFT DC | 2 |
| TFT RESET | 4 |
| SPI MOSI (shared) | 23 |
| SPI SCK (shared) | 18 |
| SPI MISO (SD only) | 19 |
| SD CS | 13 |

TFT and SD share the VSPI bus. SRX887 CS pulled LOW to enable; pull HIGH to put it in sleep mode if needed.

---

## Firmware

Arduino framework. Tested with ESP32 Arduino core 2.x.

**Dependencies:**
- `Adafruit BME280` + `Adafruit Unified Sensor`
- `TFT_eSPI` — set `User_Setup.h` for ILI9488, VSPI
- `RadioHead` — `RH_ASK` driver
- `ArduinoJson`
- `SD` (built-in)

```bash
git clone https://github.com/FilipVastl005/Mateo.git
```

### Station — `firmware/station/config.h`

```cpp
#define STATION_ID          "your-station-id"
#define TX_PIN              4
#define RAIN_PIN            27
#define SEND_INTERVAL_MIN   5    // minutes — used when delta is large
#define SEND_INTERVAL_MAX   30   // minutes — used when readings are stable
```

Wake → read sensors → diff against last tx → encode packet → transmit ASK → deep sleep. Sleep duration scales between `SEND_INTERVAL_MIN` and `SEND_INTERVAL_MAX` based on how much values changed since last send.

### Node — `firmware/node/config.h`

```cpp
#define WIFI_SSID     "ssid"
#define WIFI_PASS     "password"
#define API_KEY       "your-api-key"
#define SERVER_URL    "https://api.eggmanstudio.me/api"
#define RX_PIN        15
```

On packet receive: decode → render to TFT → POST to API. If POST fails: write to SD with ISO 8601 timestamp → retry ping every 5 min → on reconnect flush SD in order → wipe.

---

## API

Two roots, same host:

| Root | Consumer |
|------|----------|
| `https://api.eggmanstudio.me/api` | Node (ESP32) |
| `https://api.eggmanstudio.me/appapi` | Mobile apps |

All endpoints: `Authorization: Bearer <api_key>`

### Device API

**POST** `/api/readings` — submit a reading
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
Response: `{ "status": "ok", "reading_id": "r_98765" }`

**GET** `/api/ping` — connectivity check used by node before deciding to flush SD  
Response: `{ "status": "ok" }`

### App API

**GET** `/appapi/stations/{station_id}/latest`

**GET** `/appapi/stations/{station_id}/readings?from=2026-06-01&to=2026-06-18`

---

## Self-Hosting

```bash
git clone https://github.com/FilipVastl005/Mateo.git
cd Mateo/server
cp .env.example .env
docker-compose up -d
```

Or run the Node.js server directly — requires Node 18+.

---

## Registration

Create a station at [weather.eggmanstudio.me](https://weather.eggmanstudio.me) to get a `station_id` and `api_key`, then drop them into `config.h`.

---

## Apps

- Web: [weather.eggmanstudio.me](https://weather.eggmanstudio.me)
- iOS / Android: coming soon

---

## Contributing

PRs welcome — firmware, new sensor drivers, PCB layouts, server improvements.

```bash
git checkout -b feature/your-thing
# ...
git push && open PR
```

---

## License

MIT
