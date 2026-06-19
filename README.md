# Mateo — Community Weather Network

Distributed open-source weather station network. People build stations at home, data flows to a central server, apps consume it.

---

## System Overview

```
[Weather Station] --433MHz ASK--> [Home Node] --/api--> [Server] --/appapi--> [Mobile App]
                                                                  <--JWT Auth--
```

The system has two physical builds, a backend, and a mobile app:

- **Weather Station** — outdoor unit, battery/solar powered, reads sensors, transmits over 433 MHz RF
- **Home Node** — indoor unit, mains powered, receives RF from the station, shows data on a local display, forwards to the server over Wi-Fi
- **Server** — receives data from nodes, manages user accounts and auth, serves data to mobile apps
- **Mobile App** — Android (primary), iOS (planned); authenticates users, displays station data, manages station ownership

---

## Weather Station

### What it does

Runs on battery with a solar panel. Spends most of its time in deep sleep. Wakes up, reads all sensors, compares values to the last transmission, and decides how long to sleep next — between 5 and 30 minutes depending on how much the readings changed. Encodes the data into a compact ASK packet and fires it over 433 MHz RF to the node.

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

## Mobile App

Primary target: Android. iOS planned for a later stage. Both will share the same codebase (React Native or Flutter — TBD).

### What it does

- User registers and logs in with email + password
- On first launch after login, user links their `station_id` to their account
- App pulls current readings and historical data from the App API
- Displays current weather, graphs over selectable time ranges, and an air quality index derived from raw MQ-2 values
- Community map showing all public stations (optional per-station privacy toggle)
- Push notifications for significant changes (e.g. rapid pressure drop, high air quality index)

### Screens

| Screen | Description |
|--------|-------------|
| Register / Login | Email + password, JWT stored securely in device keystore |
| Dashboard | Current readings for the user's station — temp, humidity, pressure, AQI, rainfall |
| History | Time-series graphs, selectable range (24 h / 7 d / 30 d / custom) |
| Map | Community map of all public Mateo stations |
| Station settings | Rename station, toggle public/private, unlink station |
| Account settings | Change email, change password, delete account |

### Auth flow (app side)

1. `POST /appapi/auth/register` — creates account, returns JWT access token + refresh token
2. Store access token in memory, refresh token in device keystore
3. Attach `Authorization: Bearer <access_token>` to every App API request
4. On 401 response: use refresh token to get a new access token (`POST /appapi/auth/refresh`)
5. If refresh fails (expired or revoked): log user out, redirect to login

---

## Server

Hosted at `api.eggmanstudio.me`. Two API roots — one for devices, one for apps. Web dashboard at `weather.eggmanstudio.me`.

---

## User Accounts & Auth

Custom JWT-based auth. All auth endpoints are under the App API root.

### Token design

| Token | TTL | Storage |
|-------|-----|---------|
| Access token (JWT) | 15 minutes | App memory only |
| Refresh token (opaque, stored in DB) | 30 days, sliding | Device keystore |

Passwords are hashed with **bcrypt** (cost factor ≥ 12) before storage. Plain-text passwords never hit the database.

Refresh tokens are rotated on every use — each refresh issues a new refresh token and invalidates the old one. This limits the damage of a stolen token.

### Auth API endpoints — `https://api.eggmanstudio.me/appapi/auth`

**POST** `/register`
```json
{ "email": "user@example.com", "password": "..." }
```
- Validates email format, enforces minimum password length (12 chars)
- Hashes password with bcrypt
- Creates user record
- Returns access token + refresh token

**POST** `/login`
```json
{ "email": "user@example.com", "password": "..." }
```
- Verifies bcrypt hash
- Issues new access token + refresh token
- Returns both tokens

**POST** `/refresh`
```json
{ "refresh_token": "..." }
```
- Validates refresh token against DB
- Rotates token (old one invalidated immediately)
- Returns new access token + new refresh token

**POST** `/logout`
```json
{ "refresh_token": "..." }
```
- Invalidates the provided refresh token in DB
- Client discards both tokens

**POST** `/logout-all`  
Header: `Authorization: Bearer <access_token>`
- Invalidates **all** refresh tokens for the user (all devices signed out)

**POST** `/change-password`  
Header: `Authorization: Bearer <access_token>`
```json
{ "current_password": "...", "new_password": "..." }
```
- Verifies current password
- Re-hashes new password
- Invalidates all existing refresh tokens (forces re-login on all devices)

**DELETE** `/account`  
Header: `Authorization: Bearer <access_token>`
```json
{ "password": "..." }
```
- Verifies password
- Deletes user record, all associated stations, all readings, all refresh tokens

### Security rules

- Rate limiting on all auth endpoints (suggested: 10 req/min per IP on login/register)
- No password hints, no security questions
- Email enumeration protection: registration and login return identical error messages for invalid credentials
- Refresh tokens stored as hashed values in DB — raw token only ever exists in transit and on the client
- All auth endpoints HTTPS only

---

## App API — `https://api.eggmanstudio.me/appapi`

All non-auth endpoints require `Authorization: Bearer <access_token>`.

### Station management

**POST** `/stations/link`
```json
{ "station_id": "abc123" }
```
Links a station to the authenticated user's account.

**GET** `/stations` — returns all stations owned by the user

**PATCH** `/stations/{station_id}`
```json
{ "name": "Backyard", "public": true }
```

**DELETE** `/stations/{station_id}` — unlinks station from account

### Data

**GET** `/stations/{station_id}/latest` — current readings

**GET** `/stations/{station_id}/readings?from=2026-06-01&to=2026-06-18` — historical range

**GET** `/stations/public` — all public stations (for community map)

---

## Device API — `https://api.eggmanstudio.me/api`

Used by the home node only. Authenticated with a static per-station `api_key` (not JWT — nodes don't have user sessions).

**GET** `/ping` — connectivity check, returns `{ "status": "ok" }`

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

Header: `Authorization: Bearer <station_api_key>`

---

## Firmware Setup

Arduino framework, ESP32 core 2.x.

**Libraries:**
- `Adafruit BME280` + `Adafruit Unified Sensor`
- `TFT_eSPI` (configure `User_Setup.h` for ILI9488 on VSPI)
- `RadioHead` (`RH_ASK` driver)
- `ArduinoJson`
- `SD`

```bash
git clone https://github.com/FilipVastl005/Mateo.git
```

**Station config** — `firmware/station/config.h`
```cpp
#define STATION_ID          "your-station-id"
#define TX_PIN              4
#define RAIN_PIN            27
#define SEND_INTERVAL_MIN   5    // minutes
#define SEND_INTERVAL_MAX   30   // minutes
```

**Node config** — `firmware/node/config.h`
```cpp
#define WIFI_SSID     "ssid"
#define WIFI_PASS     "password"
#define API_KEY       "your-station-api-key"
#define SERVER_URL    "https://api.eggmanstudio.me/api"
#define RX_PIN        15
```

---

## Self-Hosting

```bash
cd Mateo/server
cp .env.example .env   # set DB credentials, JWT secret, bcrypt cost
docker-compose up -d
```

Node.js 18+ if running without Docker.

---

## Contributing

PRs welcome — firmware, sensor drivers, PCB layouts, server, apps.

```bash
git checkout -b feature/your-thing
```