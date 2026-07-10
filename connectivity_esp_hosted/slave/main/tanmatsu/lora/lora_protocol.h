#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LORA_PROTOCOL_VERSION_STRING_LENGTH 16

typedef enum {
    LORA_PROTOCOL_TYPE_ACK        = 0x00,
    LORA_PROTOCOL_TYPE_NACK       = 0x01,
    LORA_PROTOCOL_TYPE_GET_MODE   = 0x02,
    LORA_PROTOCOL_TYPE_SET_MODE   = 0x03,
    LORA_PROTOCOL_TYPE_GET_CONFIG = 0x04,
    LORA_PROTOCOL_TYPE_SET_CONFIG = 0x05,
    LORA_PROTOCOL_TYPE_GET_STATUS = 0x06,
    LORA_PROTOCOL_TYPE_PACKET_RX  = 0x07,
    LORA_PROTOCOL_TYPE_PACKET_TX  = 0x08,
} lora_protocol_packet_type_t;

typedef enum {
    LORA_PROTOCOL_MODE_UNKNOWN      = 0x00,
    LORA_PROTOCOL_MODE_STANDBY_RC   = 0x01,
    LORA_PROTOCOL_MODE_STANDBY_XOSC = 0x02,
    LORA_PROTOCOL_MODE_FS           = 0x03,
    LORA_PROTOCOL_MODE_TX           = 0x04,
    LORA_PROTOCOL_MODE_RX           = 0x05,
} lora_protocol_mode_t;

typedef enum {
    LORA_PROTOCOL_CHIP_SX1262 = 0x00,
    LORA_PROTOCOL_CHIP_SX1268 = 0x01,
} lora_protocol_chip_t;

typedef struct {
    lora_protocol_mode_t mode;
} __attribute__((packed)) lora_protocol_mode_params_t;

// WIRE-COMPATIBILITY WARNING: rx_boost was appended to the end of this struct
// after both sides already shipped with the fields above it. There is no
// protocol version field to gate on (same caveat as lora_protocol_rx_stats_t
// below). An old P4 host talking to a new C6 slave will send a SET_CONFIG
// payload that's sizeof(lora_protocol_config_params_t) - 1 bytes too short;
// apply_config() in lora_protocol_server.c rejects any payload shorter than
// sizeof(lora_protocol_config_params_t), so this fails loudly (ESP_ERR_INVALID_SIZE)
// rather than silently misparsing, but the C6 and P4 firmware still MUST be
// flashed together as a matching pair for LoRa config to work at all.
typedef struct {
    uint32_t frequency;                   // Frequency in Hz
    uint8_t  spreading_factor;            // 5-12
    uint16_t bandwidth;                   // 7, 10,15, 20, 31, 41, 62, 125, 250, 500 kHz
    uint8_t  coding_rate;                 // 5-8 (4/5 to 4/8)
    uint8_t  sync_word;                   // Sync word
    uint16_t preamble_length;             // Preamble length in symbols
    uint8_t  power;                       // TX Power in dBm
    uint8_t  ramp_time;                   // Microseconds
    bool     crc_enabled;                 // CRC enabled/disabled
    bool     invert_iq;                   // Invert IQ enabled/disabled
    bool     low_data_rate_optimization;  // Low data rate optimization enabled/disabled
    bool     rx_boost;                    // Boosted RX gain (+3 dB sensitivity, +~2 mA) enabled/disabled
} __attribute__((packed)) lora_protocol_config_params_t;

typedef struct {
    uint16_t             errors;
    lora_protocol_chip_t chip_type;
    char                 version_string[LORA_PROTOCOL_VERSION_STRING_LENGTH];
} __attribute__((packed)) lora_protocol_status_params_t;

typedef struct {
    uint8_t length;
    uint8_t data[];
} __attribute__((packed)) lora_protocol_lora_packet_t;

typedef struct {
    uint32_t sequence_number;
    uint32_t type;  // lora_protocol_packet_type_t
} __attribute__((packed)) lora_protocol_header_t;

// Signal quality of the most recently received packet, straight from the SX126x
// GetPacketStatus command (LoRa mode). Sent as a fixed-size block immediately
// after lora_protocol_header_t in every unsolicited LORA_PROTOCOL_TYPE_PACKET_RX
// event, followed by the raw LoRa payload bytes. There is still no explicit
// length field for the payload — its length is implied by the total esp-hosted
// custom_data length minus sizeof(lora_protocol_header_t) minus
// sizeof(lora_protocol_rx_stats_t), matching the pre-existing PACKET_RX wire
// convention (see read_data() in lora_protocol_server.c).
//
// Field meanings were cross-checked against the Semtech SX1261/62/68 datasheet
// (SS13.5.4, GetPacketStatus, LoRa mode: response bytes are
// [status, RssiPkt, SnrPkt, SignalRssiPkt]) AND against Tanmatsu's own
// meshcore firmware (components/mc_radio/radio.c in CJvanSoest/meshcore),
// which vendors this identical nicolaielectronics/sx126x driver and already
// ships a working Coverage/signal-quality feature built on exactly this
// interpretation. Note that the vendored driver's own
// sx126x_get_packet_status_lora() out-parameter names
// (out_rx_status/out_rssi_sync/out_rssi_avg) are MISLEADING: they actually
// return, in order, raw RssiPkt / raw SnrPkt / raw SignalRssiPkt — not
// "rx status", "rssi sync" or "rssi avg". Do not rename them here since that
// driver lives in a component-manager-fetched managed_components/ tree that
// isn't part of this repo (see connectivity_esp_hosted/slave/main/idf_component.yml,
// nicolaielectronics/sx126x ~0.0.3) and would be silently reset on the next
// dependency fetch; the correct interpretation is applied at the call site in
// lora_protocol_server.c instead.
//
// WIRE-COMPATIBILITY WARNING: this struct was newly inserted into the
// PACKET_RX event payload and there is no protocol version field anywhere in
// this header to gate on. The C6 slave and P4 host firmware MUST be flashed
// together as a matching pair — an old P4 host talking to a new C6 slave (or
// vice versa) will silently misparse every received LoRa packet, since these
// fields shift where the raw payload bytes start. There is no mechanism today
// to detect or prevent that skew; if one gets added later (e.g. a version
// field in lora_protocol_status_params_t), gate this struct's presence on it.
typedef struct {
    int16_t rssi_dbm;        // Packet RSSI in dBm. Raw SX126x RssiPkt byte: dbm = -raw / 2.
    int8_t  snr_db_x4;       // SNR in quarter-dB units. Raw SX126x SnrPkt is already a
                              // signed byte with LSB = 0.25 dB; actual dB = snr_db_x4 / 4.0.
                              // Kept in raw quarter-dB units (not pre-divided) to avoid
                              // losing resolution — matches Tanmatsu's own convention.
    int16_t signal_rssi_dbm; // Estimated RSSI of the signal alone, ignoring
                              // interference/blockers. Raw SX126x SignalRssiPkt byte:
                              // dbm = -raw / 2.
} __attribute__((packed)) lora_protocol_rx_stats_t;
