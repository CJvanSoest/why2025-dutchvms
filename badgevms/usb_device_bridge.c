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

/* See badgevms/include/badgevms/usb_device.h. Thin wrapper around
 * drivers/usb_device.c's internal usb_device_switch_to()/
 * usb_device_get_current_mode() (same pattern as status_led_bridge.c
 * wrapping status_led_set/show/... for apps). */

#include "badgevms/usb_device.h"
#include "drivers/usb_device.h"

bool bv_usb_device_set_mode(bv_usb_mode_t mode) {
    switch (mode) {
        case BV_USB_MODE_DEBUG: return usb_device_switch_to(USB_DEVICE_MODE_DEBUG);
        case BV_USB_MODE_BADGELINK: return usb_device_switch_to(USB_DEVICE_MODE_BADGELINK);
        case BV_USB_MODE_MSC: return usb_device_switch_to(USB_DEVICE_MODE_MSC);
        default: return false;
    }
}

bv_usb_mode_t bv_usb_device_get_mode(void) {
    switch (usb_device_get_current_mode()) {
        case USB_DEVICE_MODE_BADGELINK: return BV_USB_MODE_BADGELINK;
        case USB_DEVICE_MODE_MSC: return BV_USB_MODE_MSC;
        default: return BV_USB_MODE_DEBUG;
    }
}
