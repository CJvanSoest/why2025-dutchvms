#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Tanmatsu LoRa protocol — mirror van slave/main/tanmatsu/lora/lora_protocol.h.
 * Kept in sync handmatig voor nu; verhuist naar shared header bij 6.2b refactor. */

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
    LORA_PROTOCOL_CHIP_SX1262 = 0x00,
    LORA_PROTOCOL_CHIP_SX1268 = 0x01,
} lora_protocol_chip_t;

typedef struct {
    uint32_t sequence_number;
    uint32_t type; /* lora_protocol_packet_type_t */
} __attribute__((packed)) lora_protocol_header_t;

typedef struct {
    uint16_t errors;
    uint8_t  chip_type; /* lora_protocol_chip_t */
    char     version_string[LORA_PROTOCOL_VERSION_STRING_LENGTH];
} __attribute__((packed)) lora_protocol_status_params_t;

esp_err_t lora_proto_client_init(void);
esp_err_t lora_proto_test_echo(void);
esp_err_t lora_proto_test_get_status(void);
void      lora_proto_start_echo_test_task(void);
