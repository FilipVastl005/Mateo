/**
 * @file mesh_payload.h
 * @brief High-density packed binary telemetry struct for Topology 3 LoRa Mesh Nodes.
 * 
 * Optimized for minimum Time-on-Air (ToA) across SX1262 Sub-GHz links.
 * Employs zero padding (#pragma pack(push, 1)), bit-fields, offset scaling,
 * and CRC-16 CCITT verification.
 * 
 * Total Packet Footprint: 19 Bytes.
 */

#ifndef MESH_PAYLOAD_H
#define MESH_PAYLOAD_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_PROTOCOL_VERSION        1
#define MESH_PAYLOAD_MAGIC_OFFSET_PA 80000UL  // Offset for barometric pressure (80,000 Pa = 800 hPa)
#define MESH_AQ_SENTINEL_UNPOWERED   0xFFFF   // Sentinel for unheated MQ-135 during battery operation

typedef enum {
    PKT_TYPE_TELEMETRY = 0,
    PKT_TYPE_BEACON    = 1,
    PKT_TYPE_ROUTEREQ  = 2,
    PKT_TYPE_ACK       = 3
} MeshPacketType;

#pragma pack(push, 1)

/**
 * @brief 19-Byte Highly Packed LoRa Mesh Telemetry Packet.
 */
typedef struct {
    // Byte 0: Header & Protocol Control
    uint8_t version      : 3; // Protocol version (1..7)
    uint8_t packet_type  : 3; // MeshPacketType
    uint8_t ack_req      : 1; // 1 = Upstream node must ACK
    uint8_t reserved_hdr : 1; // Reserved for future routing extensions

    // Byte 1: Mesh Routing & TTL
    uint8_t hop_count    : 4; // Incremented at each mesh forwarder (max 15 hops)
    uint8_t max_hops     : 4; // Initial TTL

    // Byte 2: Power & Sensor Health Flags
    uint8_t is_mains_powered  : 1; // 1 = 5V mains present (MQ-135 operational)
    uint8_t is_solar_charging : 1; // 1 = TP5000 is actively charging LiFePO4
    uint8_t low_battery_warn  : 1; // 1 = Vbat < 3.0V (nearing 2.9V brownout)
    uint8_t bme_sensor_fault  : 1; // 1 = BME280 I2C NACK or CRC failure
    uint8_t mq_sensor_fault   : 1; // 1 = MQ-135 ADC out-of-range or heater fault
    uint8_t reserved_flags    : 3; // Reserved

    // Bytes 3-4: Node Identification (Mesh ID 0..65535)
    uint16_t node_id;

    // Bytes 5-6: Sequence Number (Monotonically increasing for DTN deduplication)
    uint16_t sequence_number;

    // Bytes 7-8: Temperature in centi-degrees Celsius (-50.00 C to +85.00 C)
    // Resolution: 0.01 C. Value = (int16_t)(temp_c * 100.0f)
    int16_t temperature_centi_c;

    // Bytes 9-10: Relative Humidity in centi-percent (0.00% to 100.00%)
    // Resolution: 0.01%. Value = (uint16_t)(humidity_pct * 100.0f)
    uint16_t humidity_centi_pct;

    // Bytes 11-12: Pressure in Pa offset by 80,000 Pa (Range: 800.00 to 1155.35 hPa)
    // Resolution: 1 Pa (0.01 hPa). Value = (uint16_t)(pressure_pa - 80000)
    uint16_t pressure_offset_pa;

    // Bytes 13-14: Air Quality (MQ-135) in 0.1 ppm steps (0.0 to 6553.4 ppm)
    // 0xFFFF if unpowered (battery mode). Value = (uint16_t)(ppm * 10.0f)
    uint16_t air_quality_ppm_x10;

    // Bytes 15-16: LiFePO4 Battery Voltage in millivolts (e.g. 2900 to 3650 mV)
    uint16_t battery_mv;

    // Bytes 17-18: CRC-16 CCITT (Polynomial 0x1021, Init 0xFFFF) across Bytes 0..16
    uint16_t crc16;

} MeshTelemetryPacket;

#pragma pack(pop)

// Compile-time static assert to guarantee exact 19-byte alignment across all compilers
#ifdef __cplusplus
static_assert(sizeof(MeshTelemetryPacket) == 19, "MeshTelemetryPacket must be exactly 19 bytes");
#else
_Static_assert(sizeof(MeshTelemetryPacket) == 19, "MeshTelemetryPacket must be exactly 19 bytes");
#endif

/**
 * @brief Standard CRC-16 CCITT (0x1021, Initial value 0xFFFF)
 */
static inline uint16_t mesh_calc_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

/**
 * @brief Encodes high-level physical sensor readings into packed binary struct.
 */
static inline void mesh_encode_telemetry(
    MeshTelemetryPacket *pkt,
    uint16_t node_id,
    uint16_t seq,
    uint8_t hop_count,
    uint8_t max_hops,
    float temp_c,
    float humidity_pct,
    float pressure_hpa,
    float air_quality_ppm,
    uint16_t battery_mv,
    bool is_mains,
    bool is_solar,
    bool bme_fault,
    bool mq_fault
) {
    memset(pkt, 0, sizeof(MeshTelemetryPacket));

    pkt->version = MESH_PROTOCOL_VERSION;
    pkt->packet_type = PKT_TYPE_TELEMETRY;
    pkt->ack_req = 0;
    pkt->reserved_hdr = 0;

    pkt->hop_count = hop_count & 0x0F;
    pkt->max_hops = max_hops & 0x0F;

    pkt->is_mains_powered = is_mains ? 1 : 0;
    pkt->is_solar_charging = is_solar ? 1 : 0;
    pkt->low_battery_warn = (battery_mv < 3000) ? 1 : 0;
    pkt->bme_sensor_fault = bme_fault ? 1 : 0;
    pkt->mq_sensor_fault = mq_fault ? 1 : 0;
    pkt->reserved_flags = 0;

    pkt->node_id = node_id;
    pkt->sequence_number = seq;

    // Fixed-point scaling
    pkt->temperature_centi_c = (int16_t)(temp_c * 100.0f);
    
    float clamped_hum = (humidity_pct < 0.0f) ? 0.0f : (humidity_pct > 100.0f ? 100.0f : humidity_pct);
    pkt->humidity_centi_pct = (uint16_t)(clamped_hum * 100.0f);

    uint32_t press_pa = (uint32_t)(pressure_hpa * 100.0f);
    if (press_pa >= MESH_PAYLOAD_MAGIC_OFFSET_PA) {
        pkt->pressure_offset_pa = (uint16_t)(press_pa - MESH_PAYLOAD_MAGIC_OFFSET_PA);
    } else {
        pkt->pressure_offset_pa = 0;
    }

    if (is_mains && !mq_fault && air_quality_ppm >= 0.0f) {
        pkt->air_quality_ppm_x10 = (uint16_t)(air_quality_ppm * 10.0f);
    } else {
        pkt->air_quality_ppm_x10 = MESH_AQ_SENTINEL_UNPOWERED;
    }

    pkt->battery_mv = battery_mv;

    // Calculate CRC across first 17 bytes (excluding CRC field itself)
    pkt->crc16 = mesh_calc_crc16((const uint8_t *)pkt, offsetof(MeshTelemetryPacket, crc16));
}

/**
 * @brief Validates packet integrity and extracts engineering values.
 */
static inline bool mesh_decode_telemetry(
    const uint8_t *raw_buf,
    size_t length,
    MeshTelemetryPacket *out_pkt,
    float *out_temp_c,
    float *out_hum_pct,
    float *out_press_hpa,
    float *out_aq_ppm
) {
    if (length != sizeof(MeshTelemetryPacket) || raw_buf == NULL || out_pkt == NULL) {
        return false;
    }

    memcpy(out_pkt, raw_buf, sizeof(MeshTelemetryPacket));

    // Verify CRC-16
    uint16_t expected_crc = mesh_calc_crc16(raw_buf, offsetof(MeshTelemetryPacket, crc16));
    if (out_pkt->crc16 != expected_crc) {
        return false;
    }

    // Protocol check
    if (out_pkt->version != MESH_PROTOCOL_VERSION) {
        return false;
    }

    // Unpack physical values
    if (out_temp_c) {
        *out_temp_c = (float)out_pkt->temperature_centi_c / 100.0f;
    }
    if (out_hum_pct) {
        *out_hum_pct = (float)out_pkt->humidity_centi_pct / 100.0f;
    }
    if (out_press_hpa) {
        uint32_t press_pa = (uint32_t)out_pkt->pressure_offset_pa + MESH_PAYLOAD_MAGIC_OFFSET_PA;
        *out_press_hpa = (float)press_pa / 100.0f;
    }
    if (out_aq_ppm) {
        if (out_pkt->air_quality_ppm_x10 == MESH_AQ_SENTINEL_UNPOWERED) {
            *out_aq_ppm = -1.0f; // Indicates unpowered or inactive sensor
        } else {
            *out_aq_ppm = (float)out_pkt->air_quality_ppm_x10 / 10.0f;
        }
    }

    return true;
}

#ifdef __cplusplus
}
#endif

#endif // MESH_PAYLOAD_H
