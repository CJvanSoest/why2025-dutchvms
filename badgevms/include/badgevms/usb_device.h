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

/* App-facing API for the WHY2025 carrier's bottom USB-C port personality
 * (badgevms/drivers/usb_device.c). Off/on the C6's own debug identity by
 * default; an app (e.g. cj_launcher, on the diamond key) can switch it to
 * expose BadgeLink or (once implemented, see drivers/usb_msc.c) a
 * mass-storage view of the SD card instead.
 *
 * See docs/design/badgelink-usb-port.md in the firmware repo for the
 * hardware background and current verification status. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    // Bottom USB-C port shows the C6's own native-USB debug identity.
    BV_USB_MODE_DEBUG     = 0,
    // Bottom USB-C port shows BadgeLink (16d0:0f9a) -- badgelink.py can
    // list/get/put files on SD0 and read/write NVS.
    BV_USB_MODE_BADGELINK = 1,
    // Bottom USB-C port shows the SD card as a USB mass-storage drive.
    // NOT YET IMPLEMENTED -- bv_usb_device_set_mode() always returns false
    // for this until badgevms/drivers/usb_msc.c is (see that file's own
    // comment for why this needs hardware verification with a spare SD
    // card before it's safe to enable).
    BV_USB_MODE_MSC       = 2,
} bv_usb_mode_t;

/* Request a USB personality switch. Returns false if the mode couldn't be
 * reached (currently always for BV_USB_MODE_MSC) -- the mode is unchanged
 * in that case. A no-op (returns true) if already in the requested mode. */
bool bv_usb_device_set_mode(bv_usb_mode_t mode);

/* Currently active USB personality. */
bv_usb_mode_t bv_usb_device_get_mode(void);

#ifdef __cplusplus
}
#endif
