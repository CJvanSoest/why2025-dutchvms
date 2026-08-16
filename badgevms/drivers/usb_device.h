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

#include <stdbool.h>

/* Internal (kernel-side) USB personality for the WHY2025 carrier's GPIO2
 * mux + P4 HS-OTG PHY. See usb_device_bridge.c / include/badgevms/usb_device.h
 * for the app-facing bv_usb_mode_t this maps to -- kept as a separate type so
 * this driver header doesn't have to be app-SDK-safe. */
typedef enum {
    // Mux -> C6 (default). The bottom USB-C port shows the C6's own
    // native-USB debug identity, same as before this driver existed.
    USB_DEVICE_MODE_DEBUG     = 0,
    // Mux -> P4, TinyUSB vendor class active. BadgeLink
    // (badgevms/drivers/badgelink/) reachable over USB.
    USB_DEVICE_MODE_BADGELINK = 1,
    // Mux -> P4, SD card exposed as a USB mass-storage device. See
    // usb_msc.h -- not yet wired to real storage, see that file's own
    // comment for why.
    USB_DEVICE_MODE_MSC       = 2,
} usb_device_mode_t;

/* Bring up the P4's native USB High-Speed OTG PHY behind the WHY2025
 * carrier's GPIO2-selected USB mux (shared with the C6's own native-USB
 * debug interface on the bottom USB-C connector), start BadgeLink
 * (badgevms/drivers/badgelink/) on the resulting TinyUSB vendor-class
 * device, and register it as badgelink's own SetUsbMode target. Leaves the
 * mux on its default C6 setting -- call usb_device_switch_to() to actually
 * put BadgeLink on the bus (see badgevms/usb_device_bridge.c for the
 * app-facing bv_usb_device_set_mode() this is reached through, e.g. from
 * cj_launcher's diamond-key handling).
 *
 * See docs/design/badgelink-usb-port.md for the hardware background --
 * this mux was only discovered 2026-08-16 via Senna-chan's tanmatsu-launcher
 * WHY2025 port, which is where this PHY/mux init sequence is ported from
 * (badgevms/drivers/usb_device.c has the full attribution). Build-verified,
 * hardware-boot-verified (boots cleanly with the mux flipped to the P4); the
 * bottom port's actual USB enumeration hasn't been independently observed
 * from a second machine yet.
 *
 * Returns false if the TinyUSB driver install fails. Non-fatal for boot
 * either way -- callers should log and continue, same as
 * deploy_protocol_init(). */
bool usb_device_init(void);

/* Switch the USB mux/personality. Returns false if the requested mode
 * couldn't be reached (currently: USB_DEVICE_MODE_MSC always fails, see
 * usb_msc.h). Safe to call repeatedly / with the current mode already
 * active (a no-op past the mux-toggle's own idempotency). */
bool usb_device_switch_to(usb_device_mode_t mode);

/* Currently active USB personality (USB_DEVICE_MODE_DEBUG until something
 * calls usb_device_switch_to()). */
usb_device_mode_t usb_device_get_current_mode(void);
