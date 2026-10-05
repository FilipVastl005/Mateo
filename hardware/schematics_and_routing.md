# Hardware Engineering & PCB Schematic Pin Routing Specification

## System Power, Level Translation, Thermal Isolation, and Sensor Gating
**Document Version:** 1.0.0-PROD  
**Target MCUs:** ESP32-C6 (Wi-Fi 6 Urban), ESP32-C3 (Sub-GHz Edge), ESP32-S3 (Home Deck / Solar Hub)

---

## 1. Power Subsystem Architecture

### 1.1 Solar Harvesting & Battery Charging (TP5000)
* **Solar Input:** 5.0V – 6.0V $V_{OC}$ monocrystalline panel.
* **Reverse Current Protection:** High-side P-Channel MOSFET (AO3401A, $V_{GS(th)} = -0.9\text{V}$) with gate pulled to GND via 100kΩ and 15V Zener clamp, or low-drop Schottky (SS34, $V_f \approx 0.35\text{V}$).
* **TP5000 Charger Configuration for LiFePO4:**
  * **Pin 2 (CS):** MUST BE LEFT FLOATING (or connected via solder bridge to set 3.6V LiFePO4 termination).
    * *DANGER WARNING:* If Pin 2 is pulled to GND, the TP5000 defaults to 4.2V Li-ion mode, which causes catastrophic overcharging and thermal runaway of 3.2V LiFePO4 cells!
  * **Pin 8 (BAT):** Directly connected to LiFePO4 (+) terminal through a 10µF + 100nF ceramic decoupling bank.
  * **Programming Resistor ($R_{PROG}$ on Pin 6):** Set to 0.1Ω for $I_{CHG} = 1.0\text{A}$ (or 0.2Ω for 500mA gentle charging on smaller 18650/26650 cells).
* **LiFePO4 Single Cell Characteristics:**
  * Nominal Voltage: 3.2V.
  * Full Charge Cutoff: 3.6V.
  * Brownout Deep-Sleep Threshold: 2.9V.

### 1.2 3.3V System Rail & Low-Dropout Regulation
* **Regulator Selection:** AP2112K-3.3 or TPS7A02 (Ultra-low quiescent current $I_q \approx 0.9\text{µA}$, dropout voltage $V_{drop} \approx 250\text{mV}$ at 600mA peak Wi-Fi 6 TX).
* **Rail Behavior Across Battery Curve:**
  * At $V_{BAT} = 3.6\text{V} - 3.3\text{V}$: LDO delivers clean regulated 3.30V.
  * At $V_{BAT} = 3.3\text{V} - 2.9\text{V}$: LDO enters dropout ($V_{OUT} = V_{BAT} - V_{drop} \approx 3.05\text{V} - 2.85\text{V}$).
  * *MCU Compliance:* The ESP32-C6 / ESP32-C3 operating voltage range is 3.0V to 3.6V (fully functional down to 2.7V for digital core and RF calibration tables).
  * *Threshold Action:* At 2.9V, firmware enters a protective deep-sleep brownout state, preventing voltage droop collapse.

### 1.3 Zero-Leakage Battery ADC Voltage Divider
A standard resistive divider connected directly across the battery constantly bleeds $3.2\text{V} / 2\text{M}\Omega = 1.6\text{µA}$—a 23% penalty on the 7µA deep-sleep budget.
* **Gated P-FET Circuit:**
  * **P-Channel MOSFET (Q1):** DMG2305UX ($V_{GS(th)} = -0.7\text{V}$, $SOT-23$).
    * Source -> $V_{BAT}$ (LiFePO4 +).
    * Gate -> Pulled up to $V_{BAT}$ via 100kΩ ($R_{PULL}$).
    * Drain -> Top of resistor divider ($R_{UP} = 1.0\text{M}\Omega \pm 0.1\%$).
  * **N-Channel Control MOSFET (Q2 / BSS138 or Direct GPIO):**
    * Gate connected to ESP32-C6 GPIO15 (`PIN_BAT_DIVIDER_ENABLE`).
    * Active-HIGH: Pulling GPIO15 HIGH switches Q2 ON, pulling Q1 Gate to GND, energizing the divider for 50µs.
    * Active-LOW sleep: GPIO15 LOW/tri-state isolates the divider completely (0.00µA parasitic leakage).
  * **Divider Output:** Center node between $R_{UP} (1\text{M}\Omega)$ and $R_{DOWN} (1\text{M}\Omega)$ with a 10nF ceramic filtering capacitor -> ESP32-C6 GPIO3 (ADC1_CH3).

---

## 2. 5V Mains Detection & Level Translation Circuit

* **Purpose:** Detect presence of 5V AC/DC mains adapter to safely switch on the 150mA MQ-135 heating element.
* **Input:** $V_{MAINS} = +5.0\text{V} \pm 10\%$.
* **ESP32 GPIO Limit:** Absolute Maximum $V_{IH} = 3.6\text{V}$.
* **Divider & Clamping Topology:**
  * $V_{MAINS}$ (+) -> $R_1 = 10\text{k}\Omega \pm 1\%$ -> Node `MAINS_SENSE` -> $R_2 = 15\text{k}\Omega \pm 1\%$ -> System GND.
  * Node Voltage: $V_{SENSE} = 5.0\text{V} \times \frac{15}{10 + 15} = 3.00\text{V}$ (Ideal logic HIGH for 3.3V LVCMOS).
  * **Overvoltage Protection:** 3.3V Low-capacitance Zener diode (BZX84C3V3) or BAT54S dual Schottky clamp from `MAINS_SENSE` to 3.3V rail.
  * **Debounce / Filtering:** 100nF X7R ceramic capacitor in parallel with $R_2$.
  * **Connection:** Node `MAINS_SENSE` -> ESP32-C6 GPIO5 (configured as interrupt input with external wakeup capability `ESP_EXT0`).

---

## 3. MQ-135 Gas Sensor Logic-Level MOSFET Gating

### 3.1 Heater Power Routing
* **Heater Supply ($V_H$):** Connected **EXCLUSIVELY to the +5V Mains Rail** ($V_{MAINS}$).
  * *CRITICAL RULE:* The heater MUST NEVER be connected to the LiFePO4 battery rail. At 150mA continuous, the heater would deplete a 1500mAh LiFePO4 cell in under 10 hours.
* **Heater Ground ($GND_H$):** Switched via the Low-Side N-MOSFET.

### 3.2 Low-Side N-Channel Switch Circuit
* **Transistor:** AO3400A or IRLML2502 ($SOT-23$ package).
  * Continuous Drain Current: $I_D > 4.0\text{A}$ (Heater draws 150mA).
  * Gate-to-Source Threshold: $V_{GS(th)} = 0.65\text{V} - 1.2\text{V}$.
  * On-Resistance: $R_{DS(on)} < 28\text{m}\Omega$ at $V_{GS} = 2.5\text{V}$ (negligible voltage drop across switch).
* **Connections:**
  * **Drain:** Connected to MQ-135 Heater Negative Terminal.
  * **Source:** Connected to System Power Ground plane.
  * **Gate:**
    * 220Ω series damping resistor to ESP32-C6 GPIO4 (`PIN_MQ135_HEATER_GATE`) to eliminate high-frequency ringing.
    * 100kΩ pull-down resistor from Gate to GND.
    * *FAIL-SAFE PURPOSE:* Guarantees the heater remains strictly OFF during MCU bootloader execution, software resets, and high-impedance deep-sleep states.
  * **Inductive Clamp:** 1N5819 Schottky diode connected anti-parallel across the heater pins (Cathode to +5V, Anode to Drain) to suppress inductive turn-off spikes.

### 3.3 MQ-135 Analog Output Divider
* MQ-135 analog voltage divider output ($V_{RL}$) is powered by 5V.
* Output voltage swings 0V to ~4.5V depending on gas concentration.
* **Scaling to ESP32 3.3V ADC:**
  * MQ-135 $V_{RL}$ pin -> $R_{DIV1} = 10\text{k}\Omega$ -> Node `MQ135_ADC_SCALED` -> $R_{DIV2} = 20\text{k}\Omega$ -> GND.
  * Maximum scaled voltage: $4.5\text{V} \times \frac{20}{10 + 20} = 3.00\text{V}$ (safely within ESP32-C6 ADC linear attenuation range).
  * Node `MQ135_ADC_SCALED` -> ESP32-C6 GPIO2 (ADC1_CH2).

---

## 4. BME280 I2C Interface & Thermal Isolation

### 4.1 Bus Routing & Power
* **VDD & VDDIO:** Connected to 3.3V System LDO Rail.
* **Decoupling:** 100nF (0402 ceramic) + 10µF (0603 ceramic) directly adjacent to BME280 Pin 1 and Pin 8.
* **I2C Lines:**
  * SDA -> ESP32-C6 GPIO18.
  * SCL -> ESP32-C6 GPIO19.
  * Pull-up Resistors: 4.7kΩ $\pm 1\%$ tied to 3.3V Rail.

### 4.2 Thermal Bleed Prevention (Physical PCB Layout)
* **Thermal Isolation Slot:** A 1.5mm wide, U-shaped milled mechanical PCB cutout separating the BME280 sensor sub-island from the rest of the board.
* **Conduction Choke:** The sensor sub-island connects to the main board exclusively through a 4mm-wide neck carrying only the 4 signal traces (VCC, GND, SDA, SCL).
* **Ground Plane Separation:** Ground copper pour on the sensor island connects to main ground through a single narrow thermal relief trace, preventing the ESP32 RF power amplifier thermal pulse from conducting into the sensor.
* **Distance:** Minimum 25mm clearance between ESP32 SoC/Antenna trace and BME280.

---

## 5. Master Pin Routing & Signal Allocation Table

### ESP32-C6 (Topology 1: Standalone Urban Wi-Fi 6)
| Pin / GPIO | Direction | Function / Connected Subsystem | Electrical Level |
| :--- | :--- | :--- | :--- |
| **GPIO18** | Bidirectional | BME280 I2C SDA (4.7kΩ pull-up to 3.3V) | 3.3V LVCMOS |
| **GPIO19** | Output | BME280 I2C SCL (4.7kΩ pull-up to 3.3V) | 3.3V LVCMOS |
| **GPIO4** | Output | MQ-135 N-FET Gate (220Ω series, 100kΩ pull-down) | 3.3V LVCMOS |
| **GPIO5** | Input | 5V Mains Detection Sense (10k/15k divider + Zener) | 3.0V Logic |
| **GPIO2** | Analog In | MQ-135 Scaled Gas Signal (ADC1_CH2, 10k/20k divider) | 0.0V – 3.0V Analog |
| **GPIO3** | Analog In | LiFePO4 Battery Voltage Sense (ADC1_CH3, 1M/1M divider)| 1.45V – 1.80V Analog |
| **GPIO15** | Output | Battery Divider P-FET Gate Enable (Active LOW) | 3.3V Logic |
| **RF_ANT** | RF Output | 2.4GHz Wi-Fi 6 / 802.11ax PCB Meander Antenna | 50Ω Controlled Z0 |

### ESP32-C3 & SX1262 (Topology 2 Node A & Topology 3 Mesh)
| Pin / GPIO | Direction | Function / Connected Subsystem | Electrical Level |
| :--- | :--- | :--- | :--- |
| **GPIO4** | Output | SX1262 SPI SCK | 3.3V LVCMOS |
| **GPIO5** | Output | SX1262 SPI MOSI | 3.3V LVCMOS |
| **GPIO6** | Input | SX1262 SPI MISO | 3.3V LVCMOS |
| **GPIO7** | Output | SX1262 Chip Select (NSS) | 3.3V LVCMOS |
| **GPIO3** | Input | SX1262 Busy Line | 3.3V LVCMOS |
| **GPIO8** | Input | SX1262 DIO1 (Packet Rx/Tx Interrupt) | 3.3V LVCMOS |
| **GPIO10** | Output | SX1262 Reset (NRESET) | 3.3V LVCMOS |
| **GPIO0** | Bidirectional | BME280 I2C SDA | 3.3V LVCMOS |
| **GPIO1** | Output | BME280 I2C SCL | 3.3V LVCMOS |
| **GPIO2** | Analog In | Battery Voltage Sense (ADC1_CH2) | 1.45V – 1.80V Analog |
