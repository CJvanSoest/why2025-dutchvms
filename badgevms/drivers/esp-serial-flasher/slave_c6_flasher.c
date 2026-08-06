#include "slave_c6_flasher.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp32_port.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "why2025_firmware.h"
#include "why_io.h"

static char const *TAG = "slave_c6_flasher";

/* The C6 needs real wall-clock time after esp_loader_reset_target() below to
 * finish booting its own firmware before its SDIO slave function is ready to
 * respond -- confirmed on real hardware: without this, start_wifi()'s
 * esp_wifi_init() (called right after this function returns, from the same
 * wifi_create()) tries to bring up the SDIO link too soon, fails
 * repeatedly ("sdio_card_fn_init failed" x N), and the resulting
 * ESP_ERROR_CHECK(esp_wifi_init(...)) abort()s the whole system. */
#define C6_POST_RESET_SETTLE_MS 3000

esp_err_t flash_slave_c6_if_needed() {
    // Mirrors Tanmatsu's own update flow (confirm new firmware first, only
    // reflash the radio co-processor on a later, already-stable boot):
    // skip the C6 reflash entirely while the running P4 OTA partition is
    // still ESP_OTA_IMG_PENDING_VERIFY. A crash during C6/SDIO bring-up
    // (see C6_POST_RESET_SETTLE_MS above) would otherwise happen before
    // validate_ota_partition() (run_init(), badgevms/init.c) ever gets a
    // chance to confirm this boot as valid, so ESP-IDF's bootloader rolls
    // the P4 back to the previous release on the next boot -- confirmed on
    // real hardware to reproduce every time a WiFi-OTA update also needed a
    // C6 reflash. Skipping here just defers to the next boot (the SD-staged
    // files causing the MD5 mismatch aren't touched, so this check will
    // naturally pass and proceed once this boot validates normally).
    esp_ota_img_states_t   ota_state;
    esp_partition_t const *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "P4 partition not yet confirmed valid this boot -- deferring any C6 reflash to next boot");
        return ESP_OK;
    }

    why2025_binaries_t bin;

    loader_esp32_config_t const config = {
        .baud_rate         = 115200,
        .uart_port         = UART_NUM_1,
        .uart_rx_pin       = GPIO_NUM_15,
        .uart_tx_pin       = GPIO_NUM_16,
        .reset_trigger_pin = GPIO_NUM_12,
        .gpio0_trigger_pin = GPIO_NUM_13,
    };

    if (loader_port_esp32_init(&config) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "serial initialization failed.");
        return ESP_FAIL;
    }

    if (connect_to_target_with_stub(115200, 3686400) == ESP_LOADER_SUCCESS) {
        target_chip_t target = esp_loader_get_target();
        ESP_LOGW(TAG, "Target = %d", target);
        if (target != ESP32C6_CHIP) {
            ESP_LOGE(TAG, "wrong target, expecting ESP32C6_CHIP");
            return ESP_FAIL;
        }

        // flash_binary()'s return value used to be discarded below: a genuine
        // write failure (short read after retries, a rejected packet, or the
        // stub's own post-write MD5 check failing) would print its own
        // detail via flash_binary()'s internal printf()s, but this function
        // would still fall through to "Resetting C6!"/"Done!" and return
        // ESP_OK unconditionally, as if every binary had been flashed
        // successfully. That made a real flash failure indistinguishable
        // from success in this driver's own log output, and meant
        // wifi_create() (the caller) had no way to know the C6 might still
        // be running stale/partial firmware -- see task #113/#125: this
        // alone doesn't explain every observed non-converging reflash loop
        // (a mismatched SD-card .bin/.md5 pair still won't converge no
        // matter how many times a write actually succeeds), but a silent
        // write failure was a real, separate way to end up in the same
        // "reflash every boot, LoRa never comes up" state undetected.
        // Declared before the goto below so it's always initialized at `out:`.
        bool any_write_failed = false;

        if (!get_why2025_binaries(&bin)) {
            ESP_LOGW(TAG, "Couldn't open firmware files, skipping");
            goto out;
        }

        if (esp_loader_flash_verify_known_md5(bin.boot.addr, bin.boot.size, bin.boot.md5) != ESP_LOADER_SUCCESS) {
            ESP_LOGW(TAG, "Bootloader MD5 mismatch, flashing...");
            if (flash_binary(bin.boot.fp, bin.boot.size, bin.boot.addr) != ESP_LOADER_SUCCESS) {
                ESP_LOGE(TAG, "Bootloader flash failed, C6 may be left with stale/partial firmware");
                any_write_failed = true;
            }
        } else {
            ESP_LOGW(TAG, "Bootloader MD5 match, skipping...");
        }

        if (esp_loader_flash_verify_known_md5(bin.part.addr, bin.part.size, bin.part.md5) != ESP_LOADER_SUCCESS) {
            ESP_LOGW(TAG, "Partition table MD5 mismatch, flashing...");
            if (flash_binary(bin.part.fp, bin.part.size, bin.part.addr) != ESP_LOADER_SUCCESS) {
                ESP_LOGE(TAG, "Partition table flash failed, C6 may be left with stale/partial firmware");
                any_write_failed = true;
            }
        } else {
            ESP_LOGW(TAG, "Partition table MD5 match, skipping...");
        }

        if (esp_loader_flash_verify_known_md5(bin.app.addr, bin.app.size, bin.app.md5) != ESP_LOADER_SUCCESS) {
            ESP_LOGW(TAG, "Application MD5 mismatch, flashing...");
            if (flash_binary(bin.app.fp, bin.app.size, bin.app.addr) != ESP_LOADER_SUCCESS) {
                ESP_LOGE(TAG, "Application flash failed, C6 may be left with stale/partial firmware");
                any_write_failed = true;
            }
        } else {
            ESP_LOGW(TAG, "Application MD5 match, skipping...");
        }

    out:
        ESP_LOGW(TAG, "Resetting C6!");

        esp_loader_reset_target();
        vTaskDelay(pdMS_TO_TICKS(C6_POST_RESET_SETTLE_MS));

        if (any_write_failed) {
            ESP_LOGW(TAG, "Done (with flash errors, see above)");
        } else {
            ESP_LOGW(TAG, "Done!");
        }

        free_why2025_binaries(&bin);

        return any_write_failed ? ESP_FAIL : ESP_OK;
    }

    return ESP_FAIL;
}
