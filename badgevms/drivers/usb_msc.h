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

#pragma once

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "wear_levelling.h"

#include <stdbool.h>

/* Mount the "storage" SPI-flash partition (FLASH0) via espressif/esp_tinyusb's
 * managed FAT storage helper instead of BadgeVMS's own
 * esp_vfs_fat_spiflash_mount_rw_wl() -- this is what lets USB mass-storage
 * mode later hand the same mount off to a USB host without two independent
 * FAT registrations racing each other. Must run before anything else mounts
 * or uses "/FLASH0". Call once at boot, before usb_device_init() -- this
 * only needs esp_tinyusb's MSC *driver* state (LUN table), not the TinyUSB
 * PHY/descriptor bring-up usb_device_init() does later.
 *
 * On success, *out_wl_handle is the wear-levelling handle for the "storage"
 * partition (same handle a plain wl_mount() would have returned) -- pass it
 * to fatfs_wrap_mounted_spi() (fatfs.h) to build BadgeVMS's own device_t/
 * kernel-VFS wrapper around the path this function just mounted.
 *
 * Returns false (leaving *out_wl_handle untouched) if the partition can't be
 * found/mounted, or if esp_tinyusb's MSC driver/storage-instance creation
 * fails -- callers should fall back to the old fatfs_create_spi() path in
 * that case so FLASH0 isn't left completely unavailable. */
bool usb_msc_init(char const *devname, wl_handle_t *out_wl_handle);

/* Same idea as usb_msc_init(), for SD0 instead of FLASH0 -- probes and
 * initializes the SD card (mirroring fatfs_create_sd()'s host/slot/power
 * config, see usb_msc.c) and mounts it via esp_tinyusb's SD/MMC storage
 * helper (TINYUSB_MSC_STORAGE_MOUNT_APP initially), instead of
 * fatfs_create_sd()'s own esp_vfs_fat_sdmmc_mount(). Pass the resulting
 * *out_card to fatfs_wrap_mounted_sd() (fatfs.h).
 *
 * Returns false (leaving *out_card untouched) if no card is present or any
 * step fails -- callers should fall back to fatfs_create_sd() in that case,
 * same contract as usb_msc_init()/FLASH0. Deliberately does NOT touch or
 * reuse any of fatfs_create_sd()'s own code path, so a bug here can't
 * regress SD0's existing, proven mount -- see usb_msc.c's comment. */
bool usb_msc_init_sd(char const *devname, sdmmc_card_t **out_card);

/* Switch FLASH0's (and, if usb_msc_init_sd() succeeded, SD0's) FAT mount
 * from BadgeVMS's own kernel VFS ("APP" side) to the USB host ("USB" side)
 * and back -- both storage instances switch together, exposed to the host
 * as two LUNs on the same MSC device, no separate trigger needed. Only
 * affects the FAT/VFS registration layer, not the underlying
 * wear-levelling/SD mount from usb_msc_init()/usb_msc_init_sd() -- see
 * those functions' comments. While active, "FLASH0:"/"SD0:" paths are
 * unavailable to BadgeVMS's own kernel/apps (same as unplugging a real USB
 * drive would make it unavailable locally) -- expected, not a bug.
 *
 * usb_msc_activate() returns false if FLASH0's switch fails (FLASH0 is
 * always expected to be present); SD0's switch is best-effort -- if
 * usb_msc_init_sd() never ran/failed (no card, or MSC setup failed), SD0
 * is silently skipped rather than failing the whole activate call, since
 * plenty of boots won't have a card in the slot at all.
 * usb_msc_deactivate() likewise only fails if FLASH0's switch back fails. */
bool usb_msc_activate(void);
bool usb_msc_deactivate(void);
