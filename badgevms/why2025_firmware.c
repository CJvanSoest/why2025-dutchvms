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
#error "BadgeVMS requires esp-idf 5.50 (or maybe later, who knows)"
#endif

#include "badgevms/device.h"
#include "compositor/compositor_private.h"
#include "device_private.h"
#include "drivers/badgevms_i2c_bus.h"
#include "drivers/bosch_bmi270.h"
#include "drivers/fatfs.h"
#include "drivers/st7703.h"
#include "drivers/tca8418.h"
#include "drivers/tty.h"
#include "drivers/wifi.h"
#include "esp_debug_helpers.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logical_names.h"
#include "memory.h"
#include "nvs_flash.h"
#include "ota/ota_private.h"
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
            esp_rom_printf("Crashing in task: %u\n", task_info->pid);
        } else {
            esp_rom_printf("Crashing in BadgeVMS\n");
        }

        dump_mmu();
    } else {
        esp_rom_printf("Crashing in ESP-IDF task\n");
    }

    __real_esp_panic_handler(info);
}

int app_main(void) {
    printf("BadgeVMS Initializing...\n");
    size_t free_ram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGW(TAG, "Free main memory: %zi", free_ram);

    memory_init();
    task_init();
    device_init();
    logical_names_system_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    device_register("FLASH0", fatfs_create_spi("FLASH0", "storage", true));
    // Must come after FLASH0 as that is where the firmware is
    device_register("WIFI0", wifi_create());

    device_register("PANEL0", st7703_create());
    device_register("KEYBOARD0", tca8418_keyboard_create());
    device_register("TT01", tty_create(true, true));
    device_register("I2CBUS0", badgevms_i2c_bus_create("I2CBUS0", 0, 400 * 1000));
    device_register("ORIENTATION0", bosch_bmi270_sensor_create());

    compositor_init("PANEL0", "KEYBOARD0");
    device_register("SD0", fatfs_create_sd("SD0", true));

    logical_name_set("SEARCH", "FLASH0:[SUBDIR], FLASH0:[SUBDIR.ANOTHER]", false);

    validate_ota_partition();

    printf("BadgeVMS is ready\n");
    free_ram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGW(
        TAG,
        "Free main memory: %zi, free PSRAM pages: %zi/%zi, running processes %u",
        free_ram,
        get_free_psram_pages(),
        get_total_psram_pages(),
        get_num_tasks()
    );

    char **argv = malloc(sizeof(char *) * 5);
    argv[0]     = strdup("doom.eld");
    argv[1]     = strdup("-iwad");
    argv[2]     = strdup("SD0:doom1.wad");
    // argv[2]     = strdup("FLASH0:doom1.wad");
    argv[3]     = strdup("-TIMEDEMO");
    argv[4]     = strdup("DEMO3");

    pid_t pida, pidb;

    printf("After argv stuff\n");
    free_ram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGW(
        TAG,
        "Free main memory: %zi, free PSRAM pages: %zi/%zi, running processes %u",
        free_ram,
        get_free_psram_pages(),
        get_total_psram_pages(),
        get_num_tasks()
    );

    // run_task_path("FLASH0:curl_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
    // pidb = run_task_path("FLASH0:hello.elf", 4096, TASK_TYPE_ELF_PATH, 2, argv);
    // ESP_LOGI(TAG, "Started task with pid %i", pidb);
    // pidb       = run_task(framebuffer_test_a_start, 4096, TASK_TYPE_ELF_ROM, 2, argv);
    // ESP_LOGI(TAG, "Started task with pid %i", pidb);

    // pidb = run_task_path("FLASH0:doom.elf", 4096, TASK_TYPE_ELF, 3, argv);
    // pidb = run_task_path("FLASH0:framebuffer_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
    while (1) {
        while (get_num_tasks() < 1) {
            free_ram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
            ESP_LOGW(
                TAG,
                "Free main memory: %zi, free PSRAM pages: %zi/%zi, running processes %u",
                free_ram,
                get_free_psram_pages(),
                get_total_psram_pages(),
                get_num_tasks()
            );
            pidb = run_task_path("FLASH0:process_test.elf", 4096, TASK_TYPE_ELF, 5, argv);
            // pidb = run_task_path("FLASH0:hello.elf", 4096, TASK_TYPE_ELF, 5, argv);
            // pidb = run_task_path("FLASH0:bench_basic_b.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:wifi_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:doom.elf", 4096, TASK_TYPE_ELF, 5, argv);
            // pidb = run_task_path("FLASH0:doom.elf", 4096, TASK_TYPE_ELF, 5, argv);
            // pidb = run_task_path("FLASH0:doom.elf", 4096, TASK_TYPE_ELF, 3, argv);
            // pidb = run_task_path("FLASH0:framebuffer_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:framebuffer_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:hardware_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:bmi270_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:sdl_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:sdl_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:thread_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:curl_test.elf", 4096, TASK_TYPE_ELF, 5, argv);
            // pidb = run_task_path("FLASH0:thread_test.elf", 4096, TASK_TYPE_ELF, 2, argv);
            // pidb = run_task_path("FLASH0:curl_test.elf", 4096, TASK_TYPE_ELF, 5, argv);
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }

    ESP_LOGE(TAG, "How did we get here?");
    return 0;
}
