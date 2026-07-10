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

/* See badgevms/nvs.h. NVS itself is already initialized at boot
 * (why2025_firmware.c calls nvs_flash_init()) — this file just wraps
 * nvs_open/nvs_get_blob/nvs_set_blob/nvs_commit behind a small, bounded-size
 * API PIE ELF apps can link against, same pattern drivers/wifi.c already uses
 * internally (nvs_open("badgevms_wifi", ...)) but generalised to a
 * caller-chosen namespace/key instead of one hardcoded namespace. */

#include "badgevms/nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static char const *TAG = "bv_nvs";

/* Small-blob cap — this API is for app secrets/config (channel keys, at-rest
 * encryption keys, radio settings), not bulk storage. Comfortably covers
 * everything cj_meshcore needs (its largest use is a 16-byte AES key). */
#define BV_NVS_MAX_BLOB 512

bool bv_nvs_get_blob(char const *ns, char const *key, void *out, size_t *inout_len) {
    if (!ns || !key || !out || !inout_len)
        return false;

    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READONLY, &handle) != ESP_OK)
        return false;

    uint8_t   local_buf[BV_NVS_MAX_BLOB];
    size_t    local_len = sizeof(local_buf);
    esp_err_t err       = nvs_get_blob(handle, key, local_buf, &local_len);
    nvs_close(handle);
    if (err != ESP_OK)
        return false;

    if (local_len > *inout_len) {
        ESP_LOGW(TAG, "get_blob(%s/%s): stored len %u > buffer %u", ns, key, (unsigned)local_len, (unsigned)*inout_len);
        return false;
    }

    memcpy(out, local_buf, local_len);
    *inout_len = local_len;
    return true;
}

bool bv_nvs_set_blob(char const *ns, char const *key, void const *data, size_t len) {
    if (!ns || !key || !data || len > BV_NVS_MAX_BLOB)
        return false;

    uint8_t local_buf[BV_NVS_MAX_BLOB];
    memcpy(local_buf, data, len);

    nvs_handle_t handle;
    if (nvs_open(ns, NVS_READWRITE, &handle) != ESP_OK)
        return false;

    esp_err_t err = nvs_set_blob(handle, key, local_buf, len);
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool bv_nvs_get_or_create_key(char const *ns, char const *key, uint8_t *out, size_t len) {
    if (!ns || !key || !out || len == 0 || len > 64)
        return false;

    size_t got_len = len;
    if (bv_nvs_get_blob(ns, key, out, &got_len) && got_len == len)
        return true;

    uint8_t fresh[64];
    size_t  off = 0;
    while (off < len) {
        uint32_t r     = esp_random();
        size_t   chunk = len - off < sizeof(r) ? len - off : sizeof(r);
        memcpy(&fresh[off], &r, chunk);
        off += chunk;
    }

    if (!bv_nvs_set_blob(ns, key, fresh, len)) {
        ESP_LOGE(TAG, "get_or_create_key(%s/%s): failed to persist fresh key", ns, key);
        return false;
    }

    memcpy(out, fresh, len);
    return true;
}
