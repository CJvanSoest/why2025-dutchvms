#include "lora_proto_client.h"

#include <string.h>

#include "esp_hosted.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TANMATSU_EVENT_LORA 0x01
#define TANMATSU_EVENT_ECHO 0x03

static char const       *TAG          = "lora_proto";
static SemaphoreHandle_t echo_sem     = NULL;
static uint8_t           echo_buf[64];
static size_t            echo_len     = 0;

static SemaphoreHandle_t lora_sem     = NULL;
static uint8_t           lora_buf[256];
static size_t            lora_len     = 0;
static uint32_t          lora_seq_ctr = 1;

static void echo_callback(uint32_t msg_id, uint8_t const *data, size_t data_len) {
    if (msg_id != TANMATSU_EVENT_ECHO) {
        return;
    }
    size_t n = data_len > sizeof(echo_buf) ? sizeof(echo_buf) : data_len;
    memcpy(echo_buf, data, n);
    echo_len = n;
    if (echo_sem) {
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(echo_sem, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

static void lora_callback(uint32_t msg_id, uint8_t const *data, size_t data_len) {
    if (msg_id != TANMATSU_EVENT_LORA) {
        return;
    }
    size_t n = data_len > sizeof(lora_buf) ? sizeof(lora_buf) : data_len;
    memcpy(lora_buf, data, n);
    lora_len = n;
    if (lora_sem) {
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(lora_sem, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

esp_err_t lora_proto_client_init(void) {
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGW(TAG, "lora_proto_client_init ENTER");
    if (!echo_sem) {
        echo_sem = xSemaphoreCreateBinary();
    }
    if (!lora_sem) {
        lora_sem = xSemaphoreCreateBinary();
    }
    esp_err_t res = esp_hosted_register_custom_callback(TANMATSU_EVENT_ECHO, echo_callback);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "register echo cb failed: %s", esp_err_to_name(res));
        return res;
    }
    ESP_LOGW(TAG, "echo callback registered (TANMATSU_EVENT_ECHO=0x03)");

    res = esp_hosted_register_custom_callback(TANMATSU_EVENT_LORA, lora_callback);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "register lora cb failed: %s", esp_err_to_name(res));
    } else {
        ESP_LOGW(TAG, "lora callback registered (TANMATSU_EVENT_LORA=0x01)");
    }
    return res;
}

esp_err_t lora_proto_test_get_status(void) {
    if (!lora_sem) {
        return ESP_ERR_INVALID_STATE;
    }

    lora_protocol_header_t req = {
        .sequence_number = lora_seq_ctr++,
        .type            = LORA_PROTOCOL_TYPE_GET_STATUS,
    };

    lora_len = 0;
    xSemaphoreTake(lora_sem, 0);

    esp_err_t res = esp_hosted_send_custom_data(TANMATSU_EVENT_LORA, (uint8_t const *)&req, sizeof(req));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "GET_STATUS send failed: %s", esp_err_to_name(res));
        return res;
    }
    ESP_LOGW(TAG, "GET_STATUS sent (seq=%u), waiting <=2s for response", (unsigned)req.sequence_number);

    if (xSemaphoreTake(lora_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "GET_STATUS TIMEOUT — lora_task niet responsief (mogelijk SX1262 init crashte)");
        return ESP_ERR_TIMEOUT;
    }

    if (lora_len < sizeof(lora_protocol_header_t)) {
        ESP_LOGW(TAG, "GET_STATUS reply too short: %d bytes", (int)lora_len);
        return ESP_FAIL;
    }

    lora_protocol_header_t const *reply_hdr = (lora_protocol_header_t const *)lora_buf;
    if (reply_hdr->type == LORA_PROTOCOL_TYPE_NACK) {
        ESP_LOGW(TAG, "GET_STATUS got NACK (seq=%u)", (unsigned)reply_hdr->sequence_number);
        return ESP_FAIL;
    }
    if (reply_hdr->type != LORA_PROTOCOL_TYPE_GET_STATUS) {
        ESP_LOGW(TAG, "GET_STATUS unexpected reply type 0x%02x (seq=%u)",
                 (unsigned)reply_hdr->type, (unsigned)reply_hdr->sequence_number);
        return ESP_FAIL;
    }

    size_t payload_len = lora_len - sizeof(lora_protocol_header_t);
    if (payload_len < sizeof(lora_protocol_status_params_t)) {
        ESP_LOGW(TAG, "GET_STATUS payload too short: %d bytes", (int)payload_len);
        return ESP_FAIL;
    }

    lora_protocol_status_params_t const *st =
        (lora_protocol_status_params_t const *)(lora_buf + sizeof(lora_protocol_header_t));

    char ver[LORA_PROTOCOL_VERSION_STRING_LENGTH + 1];
    memcpy(ver, st->version_string, LORA_PROTOCOL_VERSION_STRING_LENGTH);
    ver[LORA_PROTOCOL_VERSION_STRING_LENGTH] = 0;

    ESP_LOGW(TAG, "GET_STATUS OK: chip=%s errors=0x%04x version='%s'",
             st->chip_type == LORA_PROTOCOL_CHIP_SX1262 ? "SX1262"
             : st->chip_type == LORA_PROTOCOL_CHIP_SX1268 ? "SX1268" : "UNKNOWN",
             (unsigned)st->errors,
             ver);
    return ESP_OK;
}

esp_err_t lora_proto_test_echo(void) {
    if (!echo_sem) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t const ping[] = { 'W', 'H', 'Y', '-', 'P', 'I', 'N', 'G' };
    echo_len             = 0;
    xSemaphoreTake(echo_sem, 0); /* drain stale */

    esp_err_t res = esp_hosted_send_custom_data(TANMATSU_EVENT_ECHO, ping, sizeof(ping));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "send_custom_data failed: %s", esp_err_to_name(res));
        return res;
    }
    ESP_LOGW(TAG, "echo sent (%d bytes), waiting <=2s for response", (int)sizeof(ping));

    if (xSemaphoreTake(echo_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "echo TIMEOUT — C6 not responding to TANMATSU_EVENT_ECHO. "
                      "Either tanmatsu_main app_main didn't reach register_custom_callback, "
                      "or the lora_task crashed earlier.");
        return ESP_ERR_TIMEOUT;
    }

    if (echo_len != sizeof(ping) || memcmp(echo_buf, ping, sizeof(ping)) != 0) {
        ESP_LOGW(TAG, "echo MISMATCH: sent %d bytes, got %d bytes", (int)sizeof(ping), (int)echo_len);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "echo OK — C6 Tanmatsu callbacks ACTIVE (%d bytes round-trip)", (int)echo_len);
    return ESP_OK;
}

static void echo_test_task(void *arg) {
    ESP_LOGW(TAG, "echo_test_task started, sleeping 8s before tests");
    vTaskDelay(pdMS_TO_TICKS(8000)); /* let esp-hosted handshake settle */
    if (lora_proto_test_echo() == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        lora_proto_test_get_status();
    }
    vTaskDelete(NULL);
}

void lora_proto_start_echo_test_task(void) {
    BaseType_t r = xTaskCreate(echo_test_task, "lora_echo_test", 4096, NULL, 5, NULL);
    ESP_LOGW(TAG, "xTaskCreate(echo_test_task) returned %d (pdPASS=%d)", (int)r, pdPASS);
}
