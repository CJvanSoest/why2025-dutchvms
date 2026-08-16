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

/* Bring up the P4's native USB High-Speed OTG PHY behind the WHY2025
 * carrier's GPIO2-selected USB mux (shared with the C6's own native-USB
 * debug interface on the bottom USB-C connector) and start BadgeLink
 * (badgevms/drivers/badgelink/) on it as a TinyUSB vendor-class device.
 *
 * See docs/design/badgelink-usb-port.md for the hardware background --
 * this mux was only discovered 2026-08-16 via Senna-chan's tanmatsu-launcher
 * WHY2025 port, which is where this PHY/mux init sequence is ported from
 * (badgevms/drivers/usb_device.c has the full attribution). It is NOT yet
 * hardware-verified under BadgeVMS specifically -- this is the "bouwsteen 1"
 * verification spike the design doc recommends, not a finished feature.
 *
 * Returns false if the TinyUSB driver install fails. Non-fatal for boot
 * either way -- callers should log and continue, same as
 * deploy_protocol_init(). */
bool usb_device_badgelink_init(void);
