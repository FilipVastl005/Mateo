/**
 * @file main.cpp
 * @brief Topology 1: Standalone Urban Weather Station Firmware (ESP32-C6 Wi-Fi 6)
 * 
 * Key Engineering Constraints & Specifications:
 * 1. Thermal Isolation: Read BME280 immediately upon wake BEFORE RF modem PLL start.
 * 2. Power Architecture: 3.2V LiFePO4 battery; enter brownout protective deep-sleep at < 2.9V.
 * 3. Wi-Fi 6 TWT: Target Wake Time negotiation (10-minute interval, ~15ms burst TX).
 * 4. MQ-135 Heater Gating: Gated via logic-level N-FET; 5V mains detection interrupt.
 *    - On Battery: Heater OFF (0mA draw).
 *    - On 5V Mains: Heater ON, 60-second thermal stabilization, sample ADC.
 * 5. Low-Power Deep Sleep: 7µA sleep between 10-minute telemetry epochs.
 */

#include <Arduino.h>
#include <Wire.h>
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <WiFi.h>
#include <HTTPClient.h>

// ==============================================================================
// HARDWARE PIN DEFINITIONS & THRESHOLDS
// ==============================================================================
#define PIN_I2C_SDA             18
#define PIN_I2C_SCL             19
#define PIN_MQ135_HEATER_GATE   4   // Gate of Logic-Level N-MOSFET (AO3400A)
#define PIN_MAINS_5V_DETECT     5   // 5V Mains Sense via 10k/15k divider (Active HIGH)
#define PIN_MQ135_ADC           2   // ADC1 Channel 2 (MQ-135 analog output)
#define PIN_BATTERY_ADC         3   // ADC1 Channel 3 (LiFePO4 sense via gated divider)
#define PIN_BAT_DIVIDER_ENABLE  15  // P-FET gate to disable divider leakage in deep sleep

#define BME280_I2C_ADDR         0x76

// Electrical Thresholds
#define V_BAT_BROWNOUT_MV       2900  // 2.9V protective brownout deep-sleep threshold
#define V_BAT_FULL_MV           3600  // 3.6V LiFePO4 TP5000 cutoff
#define WAKEUP_INTERVAL_SEC     600   // 10 minutes = 600 seconds
#define TWT_TARGET_WAKE_TIME_US 600000000ULL // 600s in microseconds
#define MQ135_WARMUP_TIME_MS    60000 // 60s heater thermal stabilization on mains

// ==============================================================================
// RTC PERSISTENT MEMORY (Survives Deep Sleep)
// ==============================================================================
RTC_DATA_ATTR static uint32_t rtc_boot_count = 0;
RTC_DATA_ATTR static uint32_t rtc_last_seq = 0;
RTC_DATA_ATTR static bool     rtc_twt_negotiated = false;
RTC_DATA_ATTR static uint8_t  rtc_wifi_channel = 1;
RTC_DATA_ATTR static uint8_t  rtc_bssid[6] = {0};

// Sensor cache captured BEFORE RF activation
typedef struct {
    float temperature_c;
    float humidity_pct;
    float pressure_hpa;
    bool  read_success;
} ThermalIsolatedSensorData;

static ThermalIsolatedSensorData g_sensor_cache = {0.0f, 0.0f, 0.0f, false};

// ==============================================================================
// STATE MACHINE ENUMERATIONS
// ==============================================================================
enum StationState {
    STATE_COLD_WAKE,
    STATE_IMMEDIATE_SENSOR_SAMPLE,
    STATE_BATTERY_EVALUATION,
    STATE_MAINS_MQ135_SERVICING,
    STATE_WIFI6_TWT_BURST_TX,
    STATE_ENTER_DEEP_SLEEP,
    STATE_PROTECTIVE_BROWNOUT_HALT
};

static StationState g_current_state = STATE_COLD_WAKE;
static volatile bool g_mains_detected_isr = false;
static unsigned long g_mq135_heater_start_ms = 0;
static float g_mq135_ppm = -1.0f;
static uint16_t g_battery_mv = 0;

// ==============================================================================
// INTERRUPT SERVICE ROUTINES
// ==============================================================================
void IRAM_ATTR isr_mains_detect() {
    g_mains_detected_isr = (digitalRead(PIN_MAINS_5V_DETECT) == HIGH);
}

// ==============================================================================
// LOW-LEVEL DRIVER & SENSOR FUNCTIONS
// ==============================================================================

/**
 * @brief Reads LiFePO4 battery voltage via calibrated ADC.
 * Enables the high-side P-FET gate to energize the divider, samples, and turns off.
 */
uint16_t sample_battery_voltage_mv() {
    digitalWrite(PIN_BAT_DIVIDER_ENABLE, LOW); // Turn on P-MOSFET divider
    delayMicroseconds(50); // Allow RC network to stabilize

    // ESP32-C6 12-bit ADC reading
    uint32_t raw_adc = analogRead(PIN_BATTERY_ADC);
    digitalWrite(PIN_BAT_DIVIDER_ENABLE, HIGH); // Shut down divider to eliminate parasitic draw

    // Divider ratio: 1.0M upper, 1.0M lower (divide by 2).
    // V_adc = (raw_adc / 4095.0) * 3300mV * 2.0
    float v_mv = ((float)raw_adc / 4095.0f) * 3300.0f * 2.0f;
    return (uint16_t)v_mv;
}

/**
 * @brief Immediate I2C Read of BME280.
 * CRITICAL THERMAL REQUIREMENT:
 * This MUST execute before Wi-Fi modem PLL turns on. Wi-Fi 6 transmission
 * dissipates up to 800mW, thermally contaminating ambient temperature readings
 * by 1.5°C to 2.0°C within 150ms through the PCB copper plane.
 */
ThermalIsolatedSensorData read_bme280_immediate() {
    ThermalIsolatedSensorData data = {0.0f, 0.0f, 0.0f, false};

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000); // Fast mode 400kHz
    Wire.beginTransmission(BME280_I2C_ADDR);
    
    // Check chip ID (Register 0xD0 should return 0x60 for BME280)
    Wire.write(0xD0);
    if (Wire.endTransmission() != 0) {
        return data; // I2C Bus Error
    }

    Wire.requestFrom(BME280_I2C_ADDR, 1);
    if (!Wire.available() || Wire.read() != 0x60) {
        return data; // Invalid device ID
    }

    // Trigger Forced Mode Sample: osrs_t x1, osrs_p x1, osrs_h x1
    Wire.beginTransmission(BME280_I2C_ADDR);
    Wire.write(0xF2); // ctrl_hum
    Wire.write(0x01); // 1x oversampling
    Wire.endTransmission();

    Wire.beginTransmission(BME280_I2C_ADDR);
    Wire.write(0xF4); // ctrl_meas
    Wire.write(0x25); // Forced mode, 1x temp, 1x press
    Wire.endTransmission();

    // Measurement takes ~9ms in forced mode
    delay(10);

    // Read raw ADC bursts (0xF7 to 0xFE: 8 bytes)
    Wire.beginTransmission(BME280_I2C_ADDR);
    Wire.write(0xF7);
    Wire.endTransmission();

    Wire.requestFrom(BME280_I2C_ADDR, 8);
    if (Wire.available() >= 8) {
        uint32_t press_msb = Wire.read();
        uint32_t press_lsb = Wire.read();
        uint32_t press_xlsb = Wire.read();
        uint32_t temp_msb  = Wire.read();
        uint32_t temp_lsb  = Wire.read();
        uint32_t temp_xlsb = Wire.read();
        uint32_t hum_msb   = Wire.read();
        uint32_t hum_lsb   = Wire.read();

        // Bosch compensation formula conversion (Simulated compensation here)
        int32_t raw_temp = (temp_msb << 12) | (temp_lsb << 4) | (temp_xlsb >> 4);
        int32_t raw_press = (press_msb << 12) | (press_lsb << 4) | (press_xlsb >> 4);
        int32_t raw_hum = (hum_msb << 8) | hum_lsb;

        // Realistic linear calibration mapping for demonstration
        data.temperature_c = ((float)raw_temp / 16384.0f) * 10.0f; 
        data.pressure_hpa  = 800.0f + ((float)raw_press / 1048576.0f) * 300.0f;
        data.humidity_pct  = ((float)raw_hum / 65536.0f) * 100.0f;
        data.read_success  = true;
    }

    Wire.end(); // Release I2C pins and disable bus clock
    return data;
}

/**
 * @brief Configures ESP32-C6 Wi-Fi 6 Target Wake Time (TWT) Setup.
 * Establishes an Individual TWT agreement with the 802.11ax Access Point,
 * enabling sub-15ms TX bursts and scheduled wake sleep.
 */
bool setup_wifi6_twt() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);

    // Fast reconnect using cached BSSID and Channel
    wifi_config_t wifi_cfg = {};
    strcpy((char*)wifi_cfg.sta.ssid, "Urban_Weather_Net");
    strcpy((char*)wifi_cfg.sta.password, "CrowdSourced2026!");
    wifi_cfg.sta.channel = rtc_wifi_channel;
    memcpy(wifi_cfg.sta.bssid, rtc_bssid, 6);
    wifi_cfg.sta.bssid_set = (rtc_boot_count > 0);

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();
    esp_wifi_connect();

    // Configure Target Wake Time agreement
    wifi_itwt_setup_config_t twt_setup = {};
    twt_setup.setup_cmd = TWT_REQUEST;
    twt_setup.flow_type = 0; // Announced
    twt_setup.wake_int_exp = 10;
    twt_setup.wake_duration = 15; // 15 ms wake duration
    twt_setup.wake_int_mantissa = 58593; // 58593 * 2^10 us ~= 600 seconds (10 min)

    esp_err_t err = esp_wifi_sta_itwt_setup(&twt_setup);
    if (err == ESP_OK) {
        rtc_twt_negotiated = true;
        return true;
    }
#endif
    return false;
}

/**
 * @brief Rapid 15ms Telemetry Burst Dispatcher.
 */
bool transmit_telemetry_burst(
    const ThermalIsolatedSensorData *sensor,
    float mq135_ppm,
    uint16_t vbat_mv,
    bool is_mains
) {
    if (WiFi.status() != WL_CONNECTED) {
        // Quick connection attempt (timed out to 3000ms max)
        WiFi.begin("Urban_Weather_Net", "CrowdSourced2026!");
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < 3000) {
            delay(10);
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    // Cache current BSSID and channel for next fast wake
    rtc_wifi_channel = WiFi.channel();
    memcpy(rtc_bssid, WiFi.BSSID(), 6);

    HTTPClient http;
    http.begin("http://api.weather.org/api/v1/telemetry");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Station-Model", "ESP32C6-WIFI6-URBAN");

    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"seq\":%lu,\"temp\":%.2f,\"hum\":%.2f,\"press\":%.2f,\"vbat\":%u,\"aq\":%.1f,\"mains\":%s}",
        (unsigned long)rtc_last_seq++,
        sensor->temperature_c,
        sensor->humidity_pct,
        sensor->pressure_hpa,
        vbat_mv,
        mq135_ppm,
        is_mains ? "true" : "false"
    );

    int http_code = http.POST(payload);
    http.end();

    // Turn off Wi-Fi immediately to conserve power and prevent heating
    esp_wifi_stop();
    return (http_code == 200 || http_code == 201);
}

/**
 * @brief Enters ultra-low power deep sleep (7µA).
 */
void enter_deep_sleep_seconds(uint64_t seconds) {
    // Explicitly guarantee MQ-135 MOSFET is OFF during sleep
    digitalWrite(PIN_MQ135_HEATER_GATE, LOW);
    digitalWrite(PIN_BAT_DIVIDER_ENABLE, HIGH);

    // Isolate GPIOs to prevent parasitic leakage current
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
    
    // Enable 5V mains detection as external wake-up trigger
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_MAINS_5V_DETECT, 1);

    Serial.flush();
    esp_deep_sleep_start();
}

// ==============================================================================
// ARDUINO SETUP (WAKE CYCLE ENTRY)
// ==============================================================================
void setup() {
    // --------------------------------------------------------------------------
    // STEP 1: IMMEDIATE THERMAL READ (Zero MCU Heating)
    // --------------------------------------------------------------------------
    // Must execute BEFORE serial prints, ADC calibration, or RF activation
    g_sensor_cache = read_bme280_immediate();

    // Configure Pin I/O
    pinMode(PIN_BAT_DIVIDER_ENABLE, OUTPUT);
    digitalWrite(PIN_BAT_DIVIDER_ENABLE, HIGH); // Divider OFF

    pinMode(PIN_MQ135_HEATER_GATE, OUTPUT);
    digitalWrite(PIN_MQ135_HEATER_GATE, LOW);   // Heater strictly OFF by default

    pinMode(PIN_MAINS_5V_DETECT, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_MAINS_5V_DETECT), isr_mains_detect, CHANGE);

    Serial.begin(115200);
    rtc_boot_count++;

    // --------------------------------------------------------------------------
    // STEP 2: POWER & BROWNOUT EVALUATION (LiFePO4 Protective Cutoff)
    // --------------------------------------------------------------------------
    g_battery_mv = sample_battery_voltage_mv();
    bool mains_present = (digitalRead(PIN_MAINS_5V_DETECT) == HIGH);

    Serial.printf("[PWR] Boot #%lu | Vbat: %umV | Mains: %s\n", 
                  rtc_boot_count, g_battery_mv, mains_present ? "YES" : "NO");

    if (g_battery_mv < V_BAT_BROWNOUT_MV && !mains_present) {
        // Battery depleted (<2.9V). Starting Wi-Fi would cause brownout collapse.
        // Enter protective deep sleep for 1 hour to allow solar charge.
        Serial.println("[CRITICAL] LiFePO4 Brownout Protection Triggered (< 2.9V). Halting RF.");
        enter_deep_sleep_seconds(3600); // 1 Hour sleep
    }

    // --------------------------------------------------------------------------
    // STEP 3: MQ-135 AIR QUALITY DUTY CYCLING
    // --------------------------------------------------------------------------
    if (!mains_present) {
        // Battery Mode: Never energize the 150mA heater
        digitalWrite(PIN_MQ135_HEATER_GATE, LOW);
        g_mq135_ppm = -1.0f; // Indicated as disabled
        g_current_state = STATE_WIFI6_TWT_BURST_TX;
    } else {
        // Mains Mode: Energize heater and enter 60-second stabilization state
        digitalWrite(PIN_MQ135_HEATER_GATE, HIGH);
        g_mq135_heater_start_ms = millis();
        Serial.println("[MQ135] 5V Mains Active. Energizing heater for 60s thermal burn-in.");
        g_current_state = STATE_MAINS_MQ135_SERVICING;
    }
}

// ==============================================================================
// ARDUINO LOOP (DUTY-CYCLE & TRANSMISSION CONTROLLER)
// ==============================================================================
void loop() {
    switch (g_current_state) {
        case STATE_MAINS_MQ135_SERVICING: {
            // Check if 5V mains dropped during burn-in
            if (digitalRead(PIN_MAINS_5V_DETECT) == LOW) {
                Serial.println("[MQ135] Mains lost during warm-up! Disabling heater.");
                digitalWrite(PIN_MQ135_HEATER_GATE, LOW);
                g_mq135_ppm = -1.0f;
                g_current_state = STATE_WIFI6_TWT_BURST_TX;
                break;
            }

            // Non-blocking wait for 60-second thermal stabilization
            if (millis() - g_mq135_heater_start_ms >= MQ135_WARMUP_TIME_MS) {
                // Sample MQ-135 analog voltage
                uint32_t raw_adc = analogRead(PIN_MQ135_ADC);
                float v_sensor = ((float)raw_adc / 4095.0f) * 3.3f;
                
                // Sensor resistance calculation: Rs = (Vc - Vrl) * RL / Vrl
                // PPM estimation using standard curve: ppm = a * (Rs/Ro)^b
                g_mq135_ppm = (v_sensor > 0.1f) ? (100.0f * (3.3f - v_sensor) / v_sensor) : 0.0f;
                Serial.printf("[MQ135] 60s Burn-In Complete. ADC: %lu, Est PPM: %.1f\n", raw_adc, g_mq135_ppm);

                // Sample complete, shut down heater if desired or leave on while mains active
                g_current_state = STATE_WIFI6_TWT_BURST_TX;
            } else {
                delay(100); // Wait in 100ms ticks while on mains
            }
            break;
        }

        case STATE_WIFI6_TWT_BURST_TX: {
            bool mains_now = (digitalRead(PIN_MAINS_5V_DETECT) == HIGH);

            // Execute Wi-Fi 6 burst
            if (!rtc_twt_negotiated) {
                setup_wifi6_twt();
            }

            Serial.println("[WIFI6] Dispatching rapid telemetry burst...");
            bool tx_ok = transmit_telemetry_burst(&g_sensor_cache, g_mq135_ppm, g_battery_mv, mains_now);
            Serial.printf("[WIFI6] Transmission %s\n", tx_ok ? "SUCCESS" : "FAILED");

            g_current_state = STATE_ENTER_DEEP_SLEEP;
            break;
        }

        case STATE_ENTER_DEEP_SLEEP: {
            Serial.printf("[PWR] Entering 7uA Deep Sleep for %d seconds...\n", WAKEUP_INTERVAL_SEC);
            enter_deep_sleep_seconds(WAKEUP_INTERVAL_SEC);
            break;
        }

        default:
            enter_deep_sleep_seconds(WAKEUP_INTERVAL_SEC);
            break;
    }
}
