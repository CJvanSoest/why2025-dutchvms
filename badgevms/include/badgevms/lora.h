/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LORA_MAX_PACKET_LEN     255
#define LORA_VERSION_STRING_LEN 16

typedef enum {
    LORA_MODE_UNKNOWN      = 0x00,
    LORA_MODE_STANDBY_RC   = 0x01,
    LORA_MODE_STANDBY_XOSC = 0x02,
    LORA_MODE_FS           = 0x03,
    LORA_MODE_TX           = 0x04,
    LORA_MODE_RX           = 0x05,
} lora_mode_t;

typedef enum {
    LORA_CHIP_SX1262  = 0x00,
    LORA_CHIP_SX1268  = 0x01,
    LORA_CHIP_UNKNOWN = 0xff,
} lora_chip_t;

typedef struct {
    uint16_t    errors;
    lora_chip_t chip_type;
    char        version_string[LORA_VERSION_STRING_LEN + 1]; /* NUL-terminated */
} lora_status_t;

typedef struct {
    uint32_t frequency;        /* Hz */
    uint8_t  spreading_factor; /* 5-12 */
    uint16_t bandwidth;        /* kHz: 7,10,15,20,31,41,62,125,250,500 */
    uint8_t  coding_rate;      /* 5-8 (4/5 .. 4/8) */
    uint8_t  sync_word;
    uint16_t preamble_length; /* symbols */
    uint8_t  power;           /* dBm */
    uint8_t  ramp_time;       /* us */
    bool     crc_enabled;
    bool     invert_iq;
    bool     low_data_rate_optimization;
} lora_config_t;

typedef struct {
    uint8_t length;
    uint8_t data[LORA_MAX_PACKET_LEN];

    /* Signal quality of this received packet, straight from the SX126x
     * GetPacketStatus command on the C6. Only meaningful for packets returned by
     * lora_poll_packet() (i.e. actually received over the air) — not populated
     * for any other use of this struct.
     *
     * 0 dBm never occurs for a real received LoRa packet, so rssi_dbm == 0 and
     * signal_rssi_dbm == 0 double as an "unavailable" sentinel (e.g. if the C6
     * failed to query the radio for this packet); there is no separate validity
     * flag today. */
    int16_t rssi_dbm;        /* Packet RSSI in dBm. */
    int8_t  snr_db_x4;       /* SNR in quarter-dB units (LoRa SnrPkt LSB = 0.25 dB);
                              * actual dB = snr_db_x4 / 4.0. Kept unscaled to avoid
                              * losing resolution — divide in the app if you want a
                              * float dB value. */
    int16_t signal_rssi_dbm; /* Estimated RSSI of the signal alone, ignoring
                              * interference/blockers. Usually close to rssi_dbm;
                              * diverges under interference. */
} lora_packet_t;

typedef void (*lora_rx_callback_t)(lora_packet_t const *packet);

bool lora_get_status(lora_status_t *out_status);
bool lora_get_mode(lora_mode_t *out_mode);
bool lora_set_mode(lora_mode_t mode);
bool lora_get_config(lora_config_t *out_config);
bool lora_set_config(lora_config_t const *config);
bool lora_send_packet(uint8_t const *data, uint8_t length);

/* Pull next received packet from the SDK ring buffer.
 * Returns true if a packet was returned in *out, false if no packets queued.
 * Apps should call this regularly (e.g. once per UI frame). */
bool lora_poll_packet(lora_packet_t *out);

/* DEPRECATED — callback is no-op in BadgeVMS. Use lora_poll_packet() instead.
 * Cross-task calls from the esp-hosted task into PIE ELF code crash the app. */
void lora_set_rx_callback(lora_rx_callback_t callback);
