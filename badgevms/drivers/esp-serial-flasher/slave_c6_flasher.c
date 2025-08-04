
#include "slave_c6_flasher.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_loader.h"
#include "esp32_port.h"
#include "why2025_firmware.h"

static const char *TAG = "slave_c6_flasher";

esp_err_t flash_slave_c6_if_needed()
{
    why2025_binaries_t bin;

    const loader_esp32_config_t config = {
        .baud_rate = 115200,
        .uart_port = UART_NUM_1,
        .uart_rx_pin = GPIO_NUM_15,
        .uart_tx_pin = GPIO_NUM_16,
        .reset_trigger_pin = GPIO_NUM_12,
        .gpio0_trigger_pin = GPIO_NUM_13,
    };

    if (loader_port_esp32_init(&config) != ESP_LOADER_SUCCESS)
    {
        ESP_LOGE(TAG, "serial initialization failed.");
        return ESP_FAIL;
    }

    if (connect_to_target_with_stub(115200, 3686400) == ESP_LOADER_SUCCESS)
    {
        target_chip_t target = esp_loader_get_target();
        ESP_LOGI(TAG, "Target = %d", target);
        if (target != ESP32C6_CHIP)
        {
            ESP_LOGE(TAG, "wrong target, expecting ESP32C6_CHIP");
            return ESP_FAIL;
        }

        if (!get_why2025_binaries(esp_loader_get_target(), &bin)) {
            ESP_LOGW(TAG, "Couldn't open firmware files, skipping");
            free_why2025_binaries(&bin);
            return ESP_FAIL;
        }

        if (esp_loader_flash_verify_known_md5(bin.boot.addr, bin.boot.size, bin.boot.md5) != ESP_LOADER_SUCCESS)
        {
            ESP_LOGI(TAG, "Bootloader MD5 mismatch, flashing...");
            flash_binary(bin.boot.data, bin.boot.size, bin.boot.addr);
        }
        else
        {
            ESP_LOGI(TAG, "Bootloader MD5 match, skipping...");
        }

        if (esp_loader_flash_verify_known_md5(bin.part.addr, bin.part.size, bin.part.md5) != ESP_LOADER_SUCCESS)
        {
            ESP_LOGI(TAG, "Partition table MD5 mismatch, flashing...");
            flash_binary(bin.part.data, bin.part.size, bin.part.addr);
        }
        else
        {
            ESP_LOGI(TAG, "Partition table MD5 match, skipping...");
        }

        if (esp_loader_flash_verify_known_md5(bin.app.addr, bin.app.size, bin.app.md5) != ESP_LOADER_SUCCESS)
        {
            ESP_LOGI(TAG, "Application MD5 mismatch, flashing...");
            flash_binary(bin.app.data, bin.app.size, bin.app.addr);
        }
        else
        {
            ESP_LOGI(TAG, "Application MD5 match, skipping...");
        }

        ESP_LOGI(TAG, "Resetting C6!");
        esp_loader_reset_target();

        ESP_LOGI(TAG, "Done!");

        free_why2025_binaries(&bin);

        return ESP_OK;
    }

    return ESP_FAIL;
}
