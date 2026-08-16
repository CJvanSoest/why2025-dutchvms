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

/* Deliberately unimplemented -- see usb_msc.c for why. Both always return
 * false right now. Called from usb_device_switch_to(USB_DEVICE_MODE_MSC)
 * and its DEBUG/BADGELINK counterparts respectively. */
bool usb_msc_activate(void);
bool usb_msc_deactivate(void);
