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

#include "badgevms/device.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hash_helper.h"
#include "thirdparty/khash.h"

KHASH_MAP_INIT_STR(devtable, void *);

static char const *TAG = "device";

static khash_t(devtable) * device_table;
static SemaphoreHandle_t device_table_lock = NULL;

bool device_register(char const *name, device_t *device) {
    if (!device) {
        return false;
    }

    if (xSemaphoreTake(device_table_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to get device table mutex");
        abort();
    }

    khash_insert_unique_str(devtable, device_table, name, device, "The device already exists");

    xSemaphoreGive(device_table_lock);

    return true;
}

device_t *device_get(char const *name) {
    if (xSemaphoreTake(device_table_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to get device table mutex");
        abort();
    }

    khash_get_str(device_t *, device, devtable, device_table, name, "The device does not exist");

    xSemaphoreGive(device_table_lock);

    return device;
}

bool device_init() {
    ESP_LOGI(TAG, "Initializing");

    device_table      = kh_init(devtable);
    device_table_lock = xSemaphoreCreateMutex();
    if (!device_table_lock) {
        ESP_LOGE(TAG, "Failed to create device_table_lock");
        return false;
    }

    return true;
}
