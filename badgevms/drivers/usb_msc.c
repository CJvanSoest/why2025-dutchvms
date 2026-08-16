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
 * docs/design/badgelink-usb-port.md) -- deliberately NOT implemented yet.
 *
 * espressif/esp_tinyusb ships a high-level tinyusb_msc.h that can expose an
 * sdmmc_card_t as a USB drive and (via tinyusb_msc_set_storage_mount_point())
 * hand FAT ownership back and forth between "the app/kernel" and "the USB
 * host". That's the right building block, but it wants to own the SD card's
 * FAT mount itself, and badgevms/drivers/fatfs.c already mounts SD0 for
 * BadgeVMS's own use at boot. Getting that handoff wrong -- both sides
 * thinking they own the mount, or a remount racing an in-flight kernel
 * write -- risks corrupting the card, not just a crash. That needs
 * verifying against a real SD card (a spare one, not one with real data on
 * it) before this is real, not something to guess at from here.
 *
 * There's a second open question this doesn't even get to yet: whether
 * esp_tinyusb's own tinyusb_msc_install_driver() can add an MSC interface
 * to the same composite TinyUSB device usb_device.c already brings up for
 * BadgeLink's vendor class, or wants to own tinyusb_driver_install() itself.
 * usb_device.c's own vendor-only descriptor is hardware-verified as-is
 * (see docs/design/badgelink-usb-port.md) and deliberately left alone here
 * rather than risking that to add an untested second interface.
 *
 * Until both are sorted out, both functions below are honest stubs. */

#include "usb_msc.h"

#include "esp_log.h"

static char const *TAG = "usb_msc";

bool usb_msc_activate(void) {
    ESP_LOGW(TAG, "USB mass-storage isn't implemented yet -- see this file's own comment");
    return false;
}

bool usb_msc_deactivate(void) {
    return false;
}
