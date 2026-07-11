#include "lora_proto_client.h"

#include "badgevms/lora.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

/* Tanmatsu esp-hosted custom_data channel IDs.
 * Must match slave/main/tanmatsu/tanmatsu_main.c. */
#define TANMATSU_EVENT_LORA 0x01

/* Protocol mirror — kept in sync with
 * connectivity_esp_hosted/slave/main/tanmatsu/lora/lora_protocol.h */
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

typedef struct {
    uint32_t sequence_number;
    uint32_t type;
} __attribute__((packed)) lora_protocol_header_t;

typedef struct {
    /* uint32_t to match slave's enum-as-int wire format (4 bytes).
     * Master previously used uint8_t which sent 1 byte and failed slave's
     * sizeof(struct) length-check, returning NACK on every SET_MODE. */
    uint32_t mode;
} __attribute__((packed)) lora_protocol_mode_params_t;

/* WIRE-COMPATIBILITY WARNING: rx_boost was appended to the end of this struct
 * after both sides already shipped with the fields above it (same caveat as
 * lora_protocol_rx_stats_t below — no protocol version field exists). C6 and
 * P4 firmware must be flashed together as a matching pair; a size mismatch is
 * rejected loudly by apply_config() on the slave rather than silently
 * misparsed. */
typedef struct {
    uint32_t frequency;
    uint8_t  spreading_factor;
    uint16_t bandwidth;
    uint8_t  coding_rate;
    uint8_t  sync_word;
    uint16_t preamble_length;
    uint8_t  power;
    uint8_t  ramp_time;
    bool     crc_enabled;
    bool     invert_iq;
    bool     low_data_rate_optimization;
    bool     rx_boost;
} __attribute__((packed)) lora_protocol_config_params_t;

typedef struct {
    uint16_t errors;
    uint8_t  chip_type;
    char     version_string[LORA_PROTOCOL_VERSION_STRING_LENGTH];
} __attribute__((packed)) lora_protocol_status_params_t;

typedef struct {
    uint8_t length;
    uint8_t data[];
} __attribute__((packed)) lora_protocol_lora_packet_t;

/* Mirror of lora_protocol_rx_stats_t in
 * connectivity_esp_hosted/slave/main/tanmatsu/lora/lora_protocol.h — MUST match
 * that struct exactly (layout, types, order). Sent by the C6 immediately after
 * lora_protocol_header_t in every unsolicited PACKET_RX event, before the raw
 * LoRa payload bytes. See that header for the full derivation/units comment and
 * the wire-compatibility warning (no protocol version field exists — C6 and P4
 * firmware must be flashed together as a matching pair). */
typedef struct {
    int16_t rssi_dbm;        /* dBm */
    int8_t  snr_db_x4;       /* quarter-dB units; dB = snr_db_x4 / 4.0 */
    int16_t signal_rssi_dbm; /* dBm, ignoring interference/blockers */
} __attribute__((packed)) lora_protocol_rx_stats_t;

#define LORA_REPLY_BUF_SIZE   (sizeof(lora_protocol_header_t) + LORA_MAX_PACKET_LEN + 32)
#define LORA_REPLY_TIMEOUT_MS 2000

static char const *TAG = "lora_proto";

static SemaphoreHandle_t reply_sem = NULL; /* signaled when reply for outstanding_seq lands */
static SemaphoreHandle_t mutex     = NULL; /* serialize requests (only one outstanding) */
static uint8_t           reply_buf[LORA_REPLY_BUF_SIZE];
static size_t            reply_len       = 0;
static uint32_t          outstanding_seq = 0; /* 0 = no request outstanding */
static uint32_t          seq_ctr         = 1;

static lora_rx_callback_t rx_cb = NULL;

/* RX ring buffer — fills from esp-hosted task, drained by app via lora_poll_packet.
 * Storage lives in firmware static memory; safe to share across tasks.
 * Callback-based delivery (rx_cb) is unsafe in BadgeVMS because cross-task
 * calls into PIE ELF code crash the app — use polling instead. */
#define LORA_RX_RING_SLOTS 8
static lora_packet_t rx_ring[LORA_RX_RING_SLOTS];
static uint32_t volatile rx_ring_head = 0; /* write index (producer: dispatch) */
static uint32_t volatile rx_ring_tail = 0; /* read  index (consumer: poll)     */

/* PACKET_RX payload from slave is: lora_protocol_rx_stats_t followed by the raw
 * LoRa packet bytes (still NO length-prefix on the packet bytes themselves).
 * Slave sends sizeof(header) + sizeof(stats) + N total; master computes
 * N = data_len - sizeof(header) - sizeof(stats) and passes that as payload_len. */
static void dispatch_rx_packet(lora_protocol_rx_stats_t const *stats, uint8_t const *payload, size_t payload_len) {
    if (payload_len == 0 || payload_len > LORA_MAX_PACKET_LEN) {
        return;
    }

    uint32_t head = rx_ring_head;
    uint32_t next = (head + 1) % LORA_RX_RING_SLOTS;
    if (next == rx_ring_tail) {
        rx_ring_tail = (rx_ring_tail + 1) % LORA_RX_RING_SLOTS;
    }
    rx_ring[head].length          = (uint8_t)payload_len;
    rx_ring[head].rssi_dbm        = stats->rssi_dbm;
    rx_ring[head].snr_db_x4       = stats->snr_db_x4;
    rx_ring[head].signal_rssi_dbm = stats->signal_rssi_dbm;
    memcpy(rx_ring[head].data, payload, payload_len);
    rx_ring_head = next;
}

static void lora_callback(uint32_t msg_id, uint8_t const *data, size_t data_len) {
    if (msg_id != TANMATSU_EVENT_LORA) {
        return;
    }
    if (data_len < sizeof(lora_protocol_header_t)) {
        return;
    }
    lora_protocol_header_t const *hdr = (lora_protocol_header_t const *)data;

    /* PACKET_RX with seq=0 is an unsolicited event from the radio. */
    if (hdr->type == LORA_PROTOCOL_TYPE_PACKET_RX && hdr->sequence_number == 0) {
        size_t const prefix_len = sizeof(lora_protocol_header_t) + sizeof(lora_protocol_rx_stats_t);
        if (data_len < prefix_len) {
            /* Too short to contain the stats block — stale C6 firmware from before
             * this field was added, or a corrupt event. Drop it rather than reading
             * out of bounds; see the wire-compatibility warning on
             * lora_protocol_rx_stats_t (C6/P4 firmware must be paired). */
            ESP_LOGW(TAG, "PACKET_RX event too short for stats block (%u bytes)", (unsigned)data_len);
            return;
        }
        lora_protocol_rx_stats_t const *stats =
            (lora_protocol_rx_stats_t const *)(data + sizeof(lora_protocol_header_t));
        dispatch_rx_packet(stats, data + prefix_len, data_len - prefix_len);
        return;
    }

    /* Otherwise this is a reply to an outstanding request. */
    if (outstanding_seq == 0 || hdr->sequence_number != outstanding_seq) {
        ESP_LOGW(
            TAG,
            "stale/unmatched reply seq=%u type=0x%02x (outstanding=%u)",
            (unsigned)hdr->sequence_number,
            (unsigned)hdr->type,
            (unsigned)outstanding_seq
        );
        return;
    }
    size_t n = data_len > sizeof(reply_buf) ? sizeof(reply_buf) : data_len;
    memcpy(reply_buf, data, n);
    reply_len = n;
    if (reply_sem) {
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(reply_sem, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

/* Send a request and wait for a matching reply. On success, reply_buf/reply_len
 * hold the full reply (header + payload). Caller must hold `mutex`. */
static esp_err_t request_reply(uint32_t type, void const *params, size_t params_len) {
    uint32_t my_seq = seq_ctr++;
    if (my_seq == 0) {
        my_seq = seq_ctr++; /* never use 0 — that's the unsolicited PACKET_RX marker */
    }

    /* Must fit the largest possible request: PACKET_TX's params are
     * 1 (length prefix) + up to LORA_MAX_PACKET_LEN bytes (see
     * lora_send_packet() below) -- far bigger than the small fixed-size
     * control messages (GET_CONFIG/SET_CONFIG/SET_MODE) this buffer was
     * originally sized for. A too-small buffer here made this size check
     * fail *silently* (no log) for every PACKET_TX whose payload exceeded
     * the old 64-byte allowance -- e.g. every ~114-byte MeshCore advert. */
    uint8_t buf[sizeof(lora_protocol_header_t) + 1 + LORA_MAX_PACKET_LEN];
    if (sizeof(lora_protocol_header_t) + params_len > sizeof(buf)) {
        ESP_LOGE(
            TAG,
            "request type=0x%02x params_len=%u exceeds request buffer (%u)",
            (unsigned)type,
            (unsigned)params_len,
            (unsigned)sizeof(buf)
        );
        return ESP_ERR_INVALID_SIZE;
    }
    lora_protocol_header_t *hdr = (lora_protocol_header_t *)buf;
    hdr->sequence_number        = my_seq;
    hdr->type                   = type;
    if (params_len) {
        memcpy(buf + sizeof(lora_protocol_header_t), params, params_len);
    }

    reply_len       = 0;
    outstanding_seq = my_seq;
    xSemaphoreTake(reply_sem, 0); /* drain stale */

    esp_err_t res = esp_hosted_send_custom_data(TANMATSU_EVENT_LORA, buf, sizeof(lora_protocol_header_t) + params_len);
    if (res != ESP_OK) {
        outstanding_seq = 0;
        ESP_LOGE(TAG, "send type=0x%02x failed: %s", (unsigned)type, esp_err_to_name(res));
        return res;
    }

    if (xSemaphoreTake(reply_sem, pdMS_TO_TICKS(LORA_REPLY_TIMEOUT_MS)) != pdTRUE) {
        outstanding_seq = 0;
        ESP_LOGW(TAG, "request type=0x%02x seq=%u TIMEOUT", (unsigned)type, (unsigned)my_seq);
        return ESP_ERR_TIMEOUT;
    }
    outstanding_seq = 0;

    if (reply_len < sizeof(lora_protocol_header_t)) {
        ESP_LOGW(
            TAG,
            "request type=0x%02x seq=%u short reply (%u bytes, need %u)",
            (unsigned)type,
            (unsigned)my_seq,
            (unsigned)reply_len,
            (unsigned)sizeof(lora_protocol_header_t)
        );
        return ESP_FAIL;
    }
    lora_protocol_header_t const *rh = (lora_protocol_header_t const *)reply_buf;
    if (rh->type == LORA_PROTOCOL_TYPE_NACK) {
        ESP_LOGW(TAG, "request type=0x%02x got NACK", (unsigned)type);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ===== Public API ===== */

bool lora_get_status(lora_status_t *out_status) {
    if (!out_status || !mutex)
        return false;
    bool ok = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (request_reply(LORA_PROTOCOL_TYPE_GET_STATUS, NULL, 0) == ESP_OK &&
        reply_len >= sizeof(lora_protocol_header_t) + sizeof(lora_protocol_status_params_t)) {
        lora_protocol_status_params_t const *st =
            (lora_protocol_status_params_t const *)(reply_buf + sizeof(lora_protocol_header_t));
        out_status->errors    = st->errors;
        out_status->chip_type = (lora_chip_t)st->chip_type;
        memcpy(out_status->version_string, st->version_string, LORA_VERSION_STRING_LEN);
        out_status->version_string[LORA_VERSION_STRING_LEN] = '\0';
        ok                                                  = true;
    }
    xSemaphoreGive(mutex);
    return ok;
}

bool lora_get_mode(lora_mode_t *out_mode) {
    if (!out_mode || !mutex)
        return false;
    bool ok = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (request_reply(LORA_PROTOCOL_TYPE_GET_MODE, NULL, 0) == ESP_OK &&
        reply_len >= sizeof(lora_protocol_header_t) + sizeof(lora_protocol_mode_params_t)) {
        lora_protocol_mode_params_t const *mp =
            (lora_protocol_mode_params_t const *)(reply_buf + sizeof(lora_protocol_header_t));
        *out_mode = (lora_mode_t)mp->mode;
        ok        = true;
    }
    xSemaphoreGive(mutex);
    return ok;
}

bool lora_set_mode(lora_mode_t mode) {
    if (!mutex)
        return false;
    lora_protocol_mode_params_t mp = {.mode = (uint32_t)mode};
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ok = request_reply(LORA_PROTOCOL_TYPE_SET_MODE, &mp, sizeof(mp)) == ESP_OK;
    xSemaphoreGive(mutex);
    return ok;
}

bool lora_get_config(lora_config_t *out_config) {
    if (!out_config || !mutex)
        return false;
    bool ok = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (request_reply(LORA_PROTOCOL_TYPE_GET_CONFIG, NULL, 0) == ESP_OK) {
        /* rx_boost was appended to lora_protocol_config_params_t after both
         * sides had already shipped (see the WIRE-COMPATIBILITY WARNING on
         * that struct in lora_protocol.h). apply_config() on the slave
         * defends the SET_CONFIG direction (rejects a too-short payload), but
         * nothing previously defended this GET_CONFIG direction: a C6 slave
         * built before rx_boost existed replies with exactly one byte less
         * than sizeof(lora_protocol_config_params_t), and the strict >=
         * check below used to fail forever -- cfg_ok would never become
         * true again, so the Home screen's Radio line stays on "no config
         * yet - press A" even after a successful SET_CONFIG. Accept that
         * one-byte-short reply too, defaulting rx_boost to false in that
         * case, so a C6/P4 build skew degrades gracefully instead of
         * permanently hiding the config readout. */
        size_t const full_len  = sizeof(lora_protocol_header_t) + sizeof(lora_protocol_config_params_t);
        size_t const short_len = full_len - sizeof(((lora_protocol_config_params_t *)0)->rx_boost);
        if (reply_len >= short_len) {
            lora_protocol_config_params_t const *cp =
                (lora_protocol_config_params_t const *)(reply_buf + sizeof(lora_protocol_header_t));
            out_config->frequency                  = cp->frequency;
            out_config->spreading_factor           = cp->spreading_factor;
            out_config->bandwidth                  = cp->bandwidth;
            out_config->coding_rate                = cp->coding_rate;
            out_config->sync_word                  = cp->sync_word;
            out_config->preamble_length            = cp->preamble_length;
            out_config->power                      = cp->power;
            out_config->ramp_time                  = cp->ramp_time;
            out_config->crc_enabled                = cp->crc_enabled;
            out_config->invert_iq                  = cp->invert_iq;
            out_config->low_data_rate_optimization = cp->low_data_rate_optimization;
            out_config->rx_boost                   = (reply_len >= full_len) ? cp->rx_boost : false;
            ok                                     = true;
        }
    }
    xSemaphoreGive(mutex);
    return ok;
}

bool lora_set_config(lora_config_t const *config) {
    if (!config || !mutex)
        return false;
    lora_protocol_config_params_t cp = {
        .frequency                  = config->frequency,
        .spreading_factor           = config->spreading_factor,
        .bandwidth                  = config->bandwidth,
        .coding_rate                = config->coding_rate,
        .sync_word                  = config->sync_word,
        .preamble_length            = config->preamble_length,
        .power                      = config->power,
        .ramp_time                  = config->ramp_time,
        .crc_enabled                = config->crc_enabled,
        .invert_iq                  = config->invert_iq,
        .low_data_rate_optimization = config->low_data_rate_optimization,
        .rx_boost                   = config->rx_boost,
    };
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ok = request_reply(LORA_PROTOCOL_TYPE_SET_CONFIG, &cp, sizeof(cp)) == ESP_OK;
    xSemaphoreGive(mutex);
    return ok;
}

bool lora_send_packet(uint8_t const *data, uint8_t length) {
    if (!data || length == 0 || !mutex)
        return false;
    uint8_t buf[1 + LORA_MAX_PACKET_LEN];
    buf[0] = length;
    memcpy(buf + 1, data, length);
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ok = request_reply(LORA_PROTOCOL_TYPE_PACKET_TX, buf, 1 + length) == ESP_OK;
    xSemaphoreGive(mutex);
    return ok;
}

void lora_set_rx_callback(lora_rx_callback_t callback) {
    /* No-op for ELF-app safety. Callback would be invoked from esp-hosted task
     * context, which cannot safely call into PIE ELF code (crash). Use
     * lora_poll_packet() from the app's main loop instead. */
    (void)callback;
    rx_cb = NULL;
}

bool lora_poll_packet(lora_packet_t *out) {
    if (!out)
        return false;
    uint32_t tail = rx_ring_tail;
    if (tail == rx_ring_head) {
        return false; /* empty */
    }
    out->length          = rx_ring[tail].length;
    out->rssi_dbm        = rx_ring[tail].rssi_dbm;
    out->snr_db_x4       = rx_ring[tail].snr_db_x4;
    out->signal_rssi_dbm = rx_ring[tail].signal_rssi_dbm;
    memcpy(out->data, rx_ring[tail].data, out->length);
    rx_ring_tail = (tail + 1) % LORA_RX_RING_SLOTS;
    return true;
}

/* ===== Init ===== */

esp_err_t lora_proto_client_init(void) {
    if (!reply_sem)
        reply_sem = xSemaphoreCreateBinary();
    if (!mutex)
        mutex = xSemaphoreCreateMutex();

    esp_err_t res = esp_hosted_register_custom_callback(TANMATSU_EVENT_LORA, lora_callback);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "register lora cb failed: %s", esp_err_to_name(res));
        return res;
    }
    ESP_LOGW(TAG, "lora client ready (TANMATSU_EVENT_LORA=0x01)");
    return ESP_OK;
}
