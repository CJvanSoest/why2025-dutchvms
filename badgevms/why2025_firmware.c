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

#include "esp_idf_version.h"

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#error "DutchVMS requires esp-idf 5.50 (or maybe later, who knows)"
#endif

#include "application_private.h"
#include "badgevms/device.h"
#include "badgevms/notify.h"
#include "badgevms/ota.h"
#include "badgevms/process.h"
#include "badgevms_config.h"
#include "compositor/compositor_private.h"
#include "deploy_protocol.h"
#include "device_private.h"
#include "driver/uart.h"
#include "drivers/badgevms_i2c_bus.h"
#include "drivers/bosch_bme690.h"
#include "drivers/bosch_bmi270.h"
#include "drivers/fatfs.h"
#include "drivers/socket.h"
#include "drivers/st7703.h"
#include "drivers/tca8418.h"
#include "drivers/tty.h"
#include "drivers/usb_device.h"
#include "drivers/wifi.h"
#include "esp_debug_helpers.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_private/panic_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "init.h"
#include "logical_names.h"
#include "memory.h"
#include "nvs_flash.h"
#include "ota_private.h"
#include "task.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

extern void __real_esp_panic_handler(panic_info_t *info);

static char const *TAG = "why2025_main";

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info) {
    if (xTaskGetApplicationTaskTag(NULL) == (void *)0x12345678) {
        task_info_t *task_info = get_task_info();
        if (task_info && task_info->pid) {
            if (task_info->file_path) {
                esp_rom_printf("Crashing in task: %u (%s)\n", task_info->pid, task_info->file_path);
            } else {
                esp_rom_printf("Crashing in task: %u\n", task_info->pid);
            }
        } else {
            esp_rom_printf("Crashing in DutchVMS\n");
        }

        dump_mmu();
    } else {
        esp_rom_printf("Crashing in ESP-IDF task\n");
    }

    __real_esp_panic_handler(info);
}

int app_main(void) {
    esp_app_desc_t *desc = esp_ota_get_app_description();
    printf("DutchVMS version '%s' Initializing...\n", desc->version);

    size_t free_ram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGW(TAG, "Free main memory: %zi", free_ram);

    // If this fails we won't make it past here
    memory_init();

    if (!task_init()) {
        ESP_LOGE(TAG, "Failed to initialize tasking subsystem");
        invalidate_ota_partition();
    }

    notify_system_init();

    if (!device_init()) {
        ESP_LOGE(TAG, "Failed to initialize device subsystem");
        invalidate_ota_partition();
    }

    if (!logical_names_system_init()) {
        ESP_LOGE(TAG, "Failed to initialize logical names subsystem");
        invalidate_ota_partition();
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (!device_register("FLASH0", fatfs_create_spi("FLASH0", "storage", true))) {
        ESP_LOGE(TAG, "Failed to initialize FLASH0 driver");
        invalidate_ota_partition();
    }

    // Allowed to fail
    device_register("SD0", fatfs_create_sd("SD0", true));

    if (device_get("SD0")) {
        logical_name_set("STORAGE:", "SD0:, FLASH0:", false);
        logical_name_set("APPS:", "SD0:[BADGEVMS.APPS], FLASH0:[BADGEVMS.APPS]", false);
        application_init("APPS:", "SD0:[BADGEVMS.APPS]", "FLASH0:[BADGEVMS.APPS]");
    } else {
        logical_name_set("STORAGE:", "FLASH0:", false);
        logical_name_set("APPS:", "FLASH0:[BADGEVMS.APPS]", false);
        application_init("APPS:", NULL, "FLASH0:[BADGEVMS.APPS]");
    }

    if (!device_register("WIFI0", wifi_create())) {
        ESP_LOGE(TAG, "Failed to initialize WIFI0 driver");
        invalidate_ota_partition();
    }

    if (!device_register("SOCKET0", socket_create())) {
        ESP_LOGE(TAG, "Failed to initialize SOCKET0 driver");
        invalidate_ota_partition();
    }

    if (!device_register("PANEL0", st7703_create())) {
        ESP_LOGE(TAG, "Failed to initialize PANEL0 driver");
        invalidate_ota_partition();
    }

    if (!device_register("KEYBOARD0", tca8418_keyboard_create())) {
        ESP_LOGE(TAG, "Failed to initialize KEYBOARD0 driver");
        invalidate_ota_partition();
    }

    if (!device_register("TT01", tty_create(true, true))) {
        ESP_LOGE(TAG, "Failed to initialize TT01 driver");
        invalidate_ota_partition();
    }

    /* UART0 is shared by TT01 stdin (drivers/tty.c) and deploy_protocol.c
     * (BadgeLink's own UART transport was removed as non-viable on this
     * hardware — see git history) — install the interrupt-driven driver
     * once, here, before either consumer can touch UART0. Both used to do
     * raw ROM-level uart_rx_one_char() polling with no RX ring buffer,
     * which meant nothing drained the hardware FIFO while deploy_protocol's
     * PUT handler was blocked in a slow SD-card write() — a real overflow
     * risk on large file transfers. The driver's own ISR drains the FIFO
     * into RX_BUF_BYTES regardless of what any consumer task is doing.
     * No uart_param_config()/uart_set_pin() call here: UART0's pins/baud
     * are already fully configured by the IDF console/bootloader before
     * app_main() runs, and reusing that configuration is safer than
     * risking a mismatch by re-specifying it. */
#define UART0_RX_BUF_BYTES 4096
    if (uart_driver_install(UART_NUM_0, UART0_RX_BUF_BYTES, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART0 driver");
        invalidate_ota_partition();
    }

    if (!device_register("I2CBUS0", badgevms_i2c_bus_create("I2CBUS0", 0, I2C0_MASTER_FREQ_HZ))) {
        ESP_LOGE(TAG, "Failed to initialize I2CBUS0 driver");
        invalidate_ota_partition();
    }

    if (!device_register("ORIENTATION0", bosch_bmi270_sensor_create())) {
        ESP_LOGE(TAG, "Failed to initialize ORIENTATION0 driver");
        // invalidate_ota_partition();
    }

    if (!device_register("GAS0", bosch_bme690_sensor_create())) {
        ESP_LOGE(TAG, "Failed to initialize GAS0 driver");
        // invalidate_ota_partition();
    }

    if (!compositor_init("PANEL0", "KEYBOARD0")) {
        ESP_LOGE(TAG, "Failed to initialize compositor");
        invalidate_ota_partition();
    }

    logical_name_set("SEARCH", "FLASH0:[SUBDIR], FLASH0:[SUBDIR.ANOTHER]", false);

    /* CJ-PATCH: start UART deploy protocol listener (Phase A: echo stub).
     * Allowed to fail — non-critical for boot. */
    if (!deploy_protocol_init()) {
        ESP_LOGW(TAG, "deploy_protocol_init failed (non-fatal)");
    }

    /* BadgeLink over native USB (docs/design/badgelink-usb-port.md) — a
     * different physical bus from deploy_protocol_init()'s UART0, so unlike
     * the 2026-08-07 UART attempt this doesn't race with it and needs no
     * mutual-exclusion gate between the two. Still gated off by default
     * inside usb_device_badgelink_init() itself
     * (CJ_BADGEVMS_ENABLE_BADGELINK_USB) pending hardware verification —
     * see that file for why. Allowed to fail — non-critical for boot. */
    if (!usb_device_badgelink_init()) {
        ESP_LOGW(TAG, "usb_device_badgelink_init failed (non-fatal)");
    }

    /* CJ-DEBUG task #115: the ESP_LOGE/printf lines that used to sit here
     * (and throughout run_init()) NEVER reached the serial capture on
     * hardware, despite run_init()'s observable effects (apps launching)
     * clearly happening -- while esp_rom_printf() elsewhere in this codebase
     * (task.c's exception handler) reliably does. Switching to
     * esp_rom_printf() here is a differential test: a direct ROM-level UART
     * write, bypassing the ESP-IDF log/stdio buffering layers entirely, to
     * see whether THAT shows up where the normal calls didn't. */
    esp_rom_printf("CJ-DEBUG115: DutchVMS is ready\n");
    free_ram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    esp_rom_printf(
        "CJ-DEBUG115: Free main memory: %d, free PSRAM pages: %d/%d, running processes %u\n",
        (int)free_ram,
        (int)get_free_psram_pages(),
        (int)get_total_psram_pages(),
        get_num_tasks()
    );

    esp_rom_printf("CJ-DEBUG115: about to call run_init()\n");
    run_init();
    esp_rom_printf("CJ-DEBUG115: run_init() returned (should never happen in normal operation)\n");

    ESP_LOGE(TAG, "Killed init, rebooting");
    esp_restart();
    return 0;
}
