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

/* USB mass-storage (matching Senna-chan's tanmatsu-usb-msc, see
 * docs/design/badgelink-usb-port.md) -- currently wired to FLASH0 (the
 * "storage" SPI-flash partition), not SD0. All of the SCSI/block-level
 * TinyUSB callbacks (tud_msc_read10_cb() etc.) are implemented by
 * espressif/esp_tinyusb's own tinyusb_msc.c -- this file only drives its
 * high-level storage-instance API, never touches TinyUSB's MSC callbacks
 * directly.
 *
 * Deliberately scoped to FLASH0 first, not SD0: this needed a from-scratch
 * mount-ownership design (see usb_msc_init()'s comment in usb_msc.h) that
 * hadn't been verified on this firmware before, and getting SD-card mount
 * handling wrong risks corrupting whatever's on the card -- there was no
 * spare card available to safely iterate against. FLASH0 carries no
 * irreplaceable data right now (a bad mount here is a firmware bug fixed by
 * reflashing, not a data-loss risk), so it's the safer place to prove the
 * esp_tinyusb integration actually works before considering SD0. */

#include "usb_msc.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "tinyusb_msc.h"

#include <stdio.h>

static char const *TAG = "usb_msc";

static tinyusb_msc_storage_handle_t msc_handle = NULL;
static char                         msc_base_path[16];

bool usb_msc_init(char const *devname, wl_handle_t *out_wl_handle) {
    esp_partition_t const *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!partition) {
        ESP_LOGE(TAG, "No 'storage' partition found");
        return false;
    }

    // The only wl_mount() call for this partition, for the whole session --
    // esp_tinyusb's own mount-point switching (usb_msc_activate/deactivate)
    // only ever registers/unregisters the FATFS-on-top-of-this-handle
    // layer, never remounts wear-levelling itself. See storage_spiflash.c's
    // mount()/unmount() (just ff_diskio_register_wl_partition()/
    // ff_diskio_clear_pdrv_wl(), no wl_mount()/wl_unmount() calls) --
    // confirmed by reading esp_tinyusb's own source before relying on this.
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t   err       = wl_mount(partition, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wl_mount failed: %s", esp_err_to_name(err));
        return false;
    }

    tinyusb_msc_driver_config_t driver_cfg = {0};
    err                                    = tinyusb_msc_install_driver(&driver_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_install_driver failed: %s", esp_err_to_name(err));
        wl_unmount(wl_handle);
        return false;
    }

    snprintf(msc_base_path, sizeof(msc_base_path), "/%s", devname);

    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.wl_handle = wl_handle,
        .fat_fs =
            {
                .base_path = msc_base_path,
                .config =
                    {
                        .max_files              = 256,
                        .format_if_mount_failed = false,
                        .allocation_unit_size   = CONFIG_WL_SECTOR_SIZE,
                        .use_one_fat            = false,
                    },
                .do_not_format = true, // never auto-format FLASH0 out from under BadgeVMS
            },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP, // mounted for BadgeVMS's own use until activated
    };

    err = tinyusb_msc_new_storage_spiflash(&storage_cfg, &msc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_spiflash failed: %s", esp_err_to_name(err));
        msc_handle = NULL;
        wl_unmount(wl_handle);
        return false;
    }

    ESP_LOGI(TAG, "%s mounted via esp_tinyusb (app-owned)", msc_base_path);
    *out_wl_handle = wl_handle;
    return true;
}

// tinyusb_msc_set_storage_mount_point() "does not propagate failures from
// the internal mount/unmount helpers" (its own doc comment) -- it can
// return ESP_OK even when the actual mount/unmount underneath failed. Read
// the mount point back afterward and only report success if it actually
// matches what was requested, rather than trusting the return code alone.
static bool switch_mount_point(tinyusb_msc_mount_point_t target) {
    if (!msc_handle) {
        return false;
    }

    esp_err_t err = tinyusb_msc_set_storage_mount_point(msc_handle, target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_set_storage_mount_point failed: %s", esp_err_to_name(err));
        return false;
    }

    tinyusb_msc_mount_point_t actual;
    err = tinyusb_msc_get_storage_mount_point(msc_handle, &actual);
    if (err != ESP_OK || actual != target) {
        ESP_LOGE(
            TAG,
            "Mount point switch did not take effect (wanted %d, got %d, err=%s)",
            target,
            actual,
            esp_err_to_name(err)
        );
        return false;
    }

    return true;
}

bool usb_msc_activate(void) {
    return switch_mount_point(TINYUSB_MSC_STORAGE_MOUNT_USB);
}

bool usb_msc_deactivate(void) {
    if (!msc_handle) {
        return true; // usb_msc_init() never ran/succeeded -- nothing to undo
    }
    return switch_mount_point(TINYUSB_MSC_STORAGE_MOUNT_APP);
}
