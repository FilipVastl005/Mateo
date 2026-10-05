#include "mesh_payload.h"
#include <stdio.h>
#include <assert.h>
#include <math.h>

int main() {
    printf("=== Testing Topology 3 Mesh Payload Serialization ===\n");
    printf("Size of MeshTelemetryPacket: %zu bytes (Expected: 19)\n", sizeof(MeshTelemetryPacket));
    assert(sizeof(MeshTelemetryPacket) == 19);

    MeshTelemetryPacket tx_pkt;
    mesh_encode_telemetry(
        &tx_pkt,
        0xA1B2,       // node_id
        1042,         // seq
        2,            // hop_count
        7,            // max_hops
        23.45f,       // temp_c
        65.40f,       // humidity_pct
        1013.25f,     // pressure_hpa
        412.5f,       // air_quality_ppm
        3280,         // battery_mv (LiFePO4)
        true,         // is_mains
        false,        // is_solar
        false,        // bme_fault
        false         // mq_fault
    );

    // Verify raw buffer transmission
    uint8_t buffer[19];
    memcpy(buffer, &tx_pkt, sizeof(buffer));

    printf("Raw Hex: ");
    for (int i = 0; i < 19; i++) {
        printf("%02X ", buffer[i]);
    }
    printf("\n");

    // Decode and Validate
    MeshTelemetryPacket rx_pkt;
    float rx_temp = 0.0f, rx_hum = 0.0f, rx_press = 0.0f, rx_aq = 0.0f;
    bool ok = mesh_decode_telemetry(buffer, sizeof(buffer), &rx_pkt, &rx_temp, &rx_hum, &rx_press, &rx_aq);
    assert(ok == true);
    assert(rx_pkt.node_id == 0xA1B2);
    assert(rx_pkt.sequence_number == 1042);
    assert(rx_pkt.hop_count == 2);
    assert(rx_pkt.is_mains_powered == 1);
    assert(fabs(rx_temp - 23.45f) < 0.02f);
    assert(fabs(rx_hum - 65.40f) < 0.02f);
    assert(fabs(rx_press - 1013.25f) < 0.02f);
    assert(fabs(rx_aq - 412.5f) < 0.2f);
    assert(rx_pkt.battery_mv == 3280);

    // Test CRC corruption detection
    buffer[5] ^= 0xFF; // corrupt one byte
    bool corrupted_ok = mesh_decode_telemetry(buffer, sizeof(buffer), &rx_pkt, &rx_temp, &rx_hum, &rx_press, &rx_aq);
    assert(corrupted_ok == false);
    printf("CRC integrity check succeeded: Corrupted packet rejected!\n");

    printf("All Payload Assertions Passed Successfully!\n");
    return 0;
}
