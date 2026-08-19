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
 * docs/design/badgelink-usb-port.md) -- FLASH0 (the "storage" SPI-flash
 * partition) and, since usb_msc_init_sd(), SD0 too, exposed as two USB
 * LUNs. All of the SCSI/block-level TinyUSB callbacks (tud_msc_read10_cb()
 * etc.) are implemented by espressif/esp_tinyusb's own tinyusb_msc.c --
 * this file only drives its high-level storage-instance API, never touches
 * TinyUSB's MSC callbacks directly.
 *
 * Was deliberately FLASH0-only at first: SD0 needed a from-scratch
 * mount-ownership design that hadn't been verified on this firmware
 * before, and getting it wrong risks corrupting whatever's on the card.
 * FLASH0 (no irreplaceable data -- a bad mount there is a firmware bug
 * fixed by reflashing, not a data-loss risk) proved the esp_tinyusb
 * integration works first. usb_msc_init_sd() below reuses that same
 * proven mount-ownership switch (tinyusb_msc_set_storage_mount_point(),
 * see switch_mount_point()) rather than inventing a new one for SD0 --
 * it's the SD-card probe/init (host/slot/power config) that's new and
 * unverified, not the ownership-switching mechanism. See
 * usb_msc_init_sd()'s own comment for how the blast radius is kept
 * scoped: it never touches fatfs_create_sd(), SD0's existing, proven
 * mount path, which stays available as an untouched fallback. */

#include "usb_msc.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
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

/* SD0/MSC (usb_msc_init_sd()). Mirrors fatfs.c's fatfs_create_sd() pin/
 * power/slot config exactly (SD_DATA0_GPIO..SD_CMD_GPIO, the on-chip LDO
 * power-control block, 4-bit bus width, internal pull-ups) -- but as a
 * fully independent, second copy, not a shared helper. Deliberate: SD0's
 * only mount path until now (fatfs_create_sd()) is proven working across
 * this whole project's history; this is new, hardware-unverified code, and
 * a bug in it must not be able to regress that path. why2025_firmware.c
 * only calls fatfs_create_sd() as a fallback when this fails, so the two
 * never run against the same live mount at once. Keep these constants in
 * sync with fatfs.c's SD_ and SDCARD_ defines if the board's SD wiring
 * ever changes -- see fatfs.c's own copy of this comment. */
#define SD_MSC_DATA0_GPIO         39
#define SD_MSC_DATA1_GPIO         40
#define SD_MSC_DATA2_GPIO         41
#define SD_MSC_DATA3_GPIO         42
#define SD_MSC_CLK_GPIO           43
#define SD_MSC_CMD_GPIO           44
#define SD_MSC_PWR_CTRL_LDO_IO_ID 4

static sdmmc_card_t                *sd_card_handle     = NULL;
static sd_pwr_ctrl_handle_t         sd_pwr_ctrl_handle = NULL;
static tinyusb_msc_storage_handle_t sd_msc_handle      = NULL;
static char                         sd_msc_base_path[16];

bool usb_msc_init_sd(char const *devname, sdmmc_card_t **out_card) {
    // Declared up front (not at first use) so none of the error-path
    // gotos below jump into a later declaration's scope.
    sdmmc_card_t *card = NULL;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = SD_MSC_PWR_CTRL_LDO_IO_ID,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &sd_pwr_ctrl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD0 MSC: failed to create LDO power control driver: %s", esp_err_to_name(err));
        return false;
    }
    host.pwr_ctrl_handle = sd_pwr_ctrl_handle;

    err = host.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD0 MSC: host init failed: %s", esp_err_to_name(err));
        goto error_pwr;
    }

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.gpio_cd             = SDMMC_SLOT_NO_CD;
    slot_config.gpio_wp             = SDMMC_SLOT_NO_WP;
    slot_config.width               = 4;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = SD_MSC_CLK_GPIO;
    slot_config.cmd = SD_MSC_CMD_GPIO;
    slot_config.d0  = SD_MSC_DATA0_GPIO;
    slot_config.d1  = SD_MSC_DATA1_GPIO;
    slot_config.d2  = SD_MSC_DATA2_GPIO;
    slot_config.d3  = SD_MSC_DATA3_GPIO;
#endif
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = sdmmc_host_init_slot(host.slot, &slot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD0 MSC: slot init failed: %s", esp_err_to_name(err));
        goto error_host;
    }

    card = malloc(sizeof(sdmmc_card_t));
    if (!card) {
        ESP_LOGE(TAG, "SD0 MSC: failed to allocate sdmmc_card_t");
        goto error_slot;
    }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD0 MSC: no card detected or card init failed: %s", esp_err_to_name(err));
        free(card);
        goto error_slot;
    }

    tinyusb_msc_driver_config_t driver_cfg = {0};
    err                                    = tinyusb_msc_install_driver(&driver_cfg);
    // ESP_ERR_INVALID_STATE just means FLASH0's usb_msc_init() already
    // installed the (shared, driver-wide) MSC driver -- not an error here.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SD0 MSC: tinyusb_msc_install_driver failed: %s", esp_err_to_name(err));
        free(card);
        goto error_slot;
    }

    snprintf(sd_msc_base_path, sizeof(sd_msc_base_path), "/%s", devname);

    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = card,
        .fat_fs =
            {
                .base_path = sd_msc_base_path,
                .config =
                    {
                        .max_files              = 256,
                        .format_if_mount_failed = false,
                        .allocation_unit_size   = CONFIG_WL_SECTOR_SIZE,
                        .use_one_fat            = false,
                    },
                .do_not_format = true, // never auto-format SD0 out from under the user's apps
            },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP, // mounted for BadgeVMS's own use until activated
    };

    err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &sd_msc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD0 MSC: tinyusb_msc_new_storage_sdmmc failed: %s", esp_err_to_name(err));
        sd_msc_handle = NULL;
        free(card);
        goto error_slot;
    }

    ESP_LOGI(TAG, "%s mounted via esp_tinyusb (app-owned)", sd_msc_base_path);
    sd_card_handle = card;
    *out_card      = card;
    return true;

error_slot:
    sdmmc_host_deinit_slot(host.slot);
error_host:
    sdmmc_host_deinit();
error_pwr:
    sd_pwr_ctrl_del_on_chip_ldo(sd_pwr_ctrl_handle);
    sd_pwr_ctrl_handle = NULL;
    return false;
}

// tinyusb_msc_set_storage_mount_point() "does not propagate failures from
// the internal mount/unmount helpers" (its own doc comment) -- it can
// return ESP_OK even when the actual mount/unmount underneath failed. Read
// the mount point back afterward and only report success if it actually
// matches what was requested, rather than trusting the return code alone.
static bool switch_mount_point(tinyusb_msc_storage_handle_t handle, tinyusb_msc_mount_point_t target) {
    if (!handle) {
        return false;
    }

    esp_err_t err = tinyusb_msc_set_storage_mount_point(handle, target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_set_storage_mount_point failed: %s", esp_err_to_name(err));
        return false;
    }

    tinyusb_msc_mount_point_t actual;
    err = tinyusb_msc_get_storage_mount_point(handle, &actual);
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
    bool ok = switch_mount_point(msc_handle, TINYUSB_MSC_STORAGE_MOUNT_USB);
    // SD0 is best-effort -- plenty of boots won't have a card in the slot
    // at all (usb_msc_init_sd() never ran/succeeded, sd_msc_handle stays
    // NULL, switch_mount_point() no-ops via its !handle check above), and
    // that must not fail the whole activate call.
    if (sd_msc_handle && !switch_mount_point(sd_msc_handle, TINYUSB_MSC_STORAGE_MOUNT_USB)) {
        ESP_LOGW(TAG, "SD0 MSC activate failed, exposing FLASH0 only");
    }
    return ok;
}

bool usb_msc_deactivate(void) {
    if (sd_msc_handle && !switch_mount_point(sd_msc_handle, TINYUSB_MSC_STORAGE_MOUNT_APP)) {
        ESP_LOGW(TAG, "SD0 MSC deactivate failed, SD0: may stay unavailable to BadgeVMS");
    }
    if (!msc_handle) {
        return true; // usb_msc_init() never ran/succeeded -- nothing to undo
    }
    return switch_mount_point(msc_handle, TINYUSB_MSC_STORAGE_MOUNT_APP);
}
