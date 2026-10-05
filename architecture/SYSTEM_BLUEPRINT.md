# Crowdsourced Open-Source Weather Ecosystem: System Architecture Blueprint

## Architectural Overview & Topology Breakdown

```
+---------------------------------------------------------------------------------------------------------+
|                                           CLOUD INFRASTRUCTURE                                          |
|                                                                                                         |
|  [Ingestion API] <----+---+                                                                             |
|  FastAPI / NestJS     |   |                                                                             |
|  Zero-Trust Ed25519   |   |                                                                             |
|                       |   |                                                                             |
|  [Multi-Tier QC Engine]   |                                                                             |
|  - Tier 1: Hardware Limits|                                                                             |
|  - Tier 2: Temporal Delta |                                                                             |
|  - Tier 3: Spatial IDW    |                                                                             |
|  - Tier 4: SQI Demotion   |                                                                             |
|            |              |                                                                             |
|  [TimescaleDB + PostGIS]  |                                                                             |
|  - 7-Day Chunk Hypertables|                                                                             |
|  - 15m/1h Continuous Aggr |                                                                             |
|  - Columnar Compression   |                                                                             |
|  - 90-Day Retention       |                                                                             |
+------------^--------------+----------------------------------^------------------------------------------+
             | TLS 1.3 / HTTPS                                 | MQTT / HTTPS
             |                                                 |
+------------+-------------+              +--------------------+---------------------+
| TOPOLOGY 1: URBAN        |              | TOPOLOGY 2: SUBURBAN HOME DECK           |
| ESP32-C6 (Wi-Fi 6)       |              | Node B: ESP32-S3 (Mains Powered Hub)     |
| - Target Wake Time (TWT) |              | - Slotted TDMA Master Scheduler          |
| - 10-Minute Wake Interval|              | - SD-Card CSV (FAT32 Corruption-Resistant|
| - 15ms Burst TX          |              | - Local Web Server (homedeck.local)      |
| - 7µA Deep Sleep         |              | - Home Assistant MQTT Discovery          |
| - Immediate BME280 Read  |              +-------------------^----------------------+
| - MQ-135 150mA N-FET Gate|                                  | LoRa 868/915MHz
| - 2.9V Brownout Halt     |                                  | Slotted TDMA + AES-GCM
+--------------------------+                      +-----------+-------------+
                                                  | Node A: Outdoor Edge    |
                                                  | ESP32-C3 + SX1262 LoRa  |
                                                  | - 5-min RTC Wake        |
                                                  | - 2s TDMA Sync Window   |
                                                  | - LiFePO4 + TP5000 Solar|
                                                  +-------------------------+

+---------------------------------------------------------------------------------------------------------+
| TOPOLOGY 3: AUTONOMOUS OFF-GRID MESH (Remote / Disaster-Resilient)                                      |
|                                                                                                         |
|  +--------------------+         CSMA/CA LoRa Mesh        +--------------------+                         |
|  | Mesh Edge Node 1   | <------------------------------> | Mesh Edge Node 2   |                         |
|  | ESP32-C3 + SX1262  |   Packed 19-Byte C-Struct        | ESP32-C3 + SX1262  |                         |
|  +---------+----------+                                  +----------+---------+                         |
|            |                                                        |                                   |
|            +----------------------------+---------------------------+                                   |
|                                         | LoRa Multi-Hop Forwarding                                     |
|                                         v                                                               |
|                         +-----------------------------------+                                           |
|                         | Community Solar Hub (ESP32-S3 / Pi|                                           |
|                         | - Captive Portal (http://community|                                           |
|                         | - Offline-First Local Dashboard   |                                           |
|                         | - Delay-Tolerant Networking (DTN) |                                           |
|                         | - Sneakernet Export /sync API     |                                           |
|                         +-----------------------------------+                                           |
+---------------------------------------------------------------------------------------------------------+
```

---

## 1. Physical Layer & Thermal Dynamics

### Thermal Bleed Countermeasure (Zero-Error Ambient Sampling)
When an IoT microcontroller initializes its 2.4GHz Wi-Fi or Sub-GHz RF amplifier (+20dBm to +22dBm output), power consumption spikes from microamps to over 200–350mA. This generates an instantaneous thermal dissipation of 600mW to 1.1W.
Because copper pours on a compact PCB exhibit high thermal conductivity, this thermal pulse travels to an on-board temperature sensor within 100–250ms, introducing a false positive bias.

**Enforced Mitigation Protocol:**
1. **Cold Boot Entry:** The MCU resets/wakes directly into a bare-metal register state.
2. **Immediate Sampling:** Before initializing the RF PLL, Wi-Fi stack, radio calibration tables, serial buffers, or high-speed system clocks, the MCU activates the I2C bus at 400kHz.
3. **Single-Shot Forced Read:** The BME280 is triggered into *Forced Mode*, completes conversion in 9.3ms, and dumps raw 20-bit calibration frames.
4. **I2C Shutdown:** The I2C bus is immediately de-asserted and floating pins are parked.
5. **Modem Initialization:** Only *after* ambient values are locked into volatile RTC memory is the RF subsystem powered on.

---

## 2. Power Architecture & Battery Chemistry

### LiFePO4 vs. Standard Li-ion
* **Nominal Voltage:** 3.2V (extremely flat discharge plateau between 3.25V and 3.15V).
* **Charge Termination Voltage:** Exactly 3.60V (managed by TP5000 with Pin 2 CS floated).
* **Thermal & Cycle Life:** 3,000 to 5,000 cycles without dendrite formation; safe operation from $-20^\circ\text{C}$ to $+65^\circ\text{C}$.
* **Brownout Protective Cutoff:** 2.90V.
  * Below 2.90V, cell internal resistance ($R_i$) rises exponentially.
  * Attempting an RF transmission pulse (300mA peak) at $<2.90\text{V}$ causes an IR voltage drop:
    $$V_{rail} = V_{cell} - (I_{peak} \times R_i) < 2.50\text{V}$$
    This triggers a hardware brownout reset loop (BOR), causing non-volatile memory corruption.
  * **Protective Logic:** Firmware samples battery voltage before radio spin-up. If $V_{BAT} < 2.90\text{V}$, the system aborts RF activation and forces deep sleep for 1 hour to allow solar recovery.

---

## 3. Topologies & Communication Protocols

### Topology 1: Standalone Urban Wi-Fi 6 (ESP32-C6)
* Utilizes **Wi-Fi 6 (802.11ax) Target Wake Time (TWT)**.
* Instead of maintaining a DTIM listen interval, the client negotiates a periodic Individual TWT contract with the 802.11ax Access Point.
* Target Wake Time: Every 600 seconds.
* Nominal Wake Duration: 15ms.
* Sleep Current: 7µA.

### Topology 2: Suburban Home Deck (ESP32-C3 + ESP32-S3)
* **Node A (Edge):** ESP32-C3 + SX1262 LoRa. Wakes every 5 minutes.
* **TDMA Protocol:** Node B (Home Deck Hub) broadcasts a high-precision synchronization beacon every 300 seconds. Node A aligns its crystal timer, waits for its pre-assigned 50ms time slot, transmits an AES-GCM-128 encrypted telemetry packet, awaits an immediate ACK, and sleeps.
* **Node B (Hub):** Mains powered.
  * Hosts a local mDNS web server (`http://homedeck.local`).
  * Writes to SD Card in append-only CSV mode with a write-ahead log (WAL) and sector flush to prevent FAT32 file allocation table corruption during sudden power drops.
  * Publishes MQTT Home Assistant discovery payloads (`homeassistant/sensor/weather_.../config`).

### Topology 3: Autonomous Off-Grid Mesh (ESP32-C3 + S3/Pi Zero)
* Operates in remote valleys or disaster areas without cellular or Wi-Fi infrastructure.
* Uses non-persistent CSMA/CA with Channel Activity Detection (CAD) on SX1262 before transmission.
* Encodes telemetry in an ultra-dense 19-byte C-struct with CRC-16 CCITT.
* Community Solar Hub stores all packets in an embedded SQLite/DuckDB queue with a 30-day circular buffer.
* **Delay-Tolerant Networking (DTN) Sneakernet Sync:**
  * When rangers, drones, or local residents connect to the captive Wi-Fi AP (`http://community.weather`), their client browser/tool requests `/api/v1/export/sync?since=<seq>`.
  * The hub packages compressed differential telemetry.
  * Once the field worker reaches internet connectivity, their device automatically syncs the bundle upstream to the central cloud.
