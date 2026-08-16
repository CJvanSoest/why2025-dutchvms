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

#include "badgevms/device.h"

device_t *fatfs_create_spi(char const *devname, char const *partname, bool rw);
device_t *fatfs_create_sd(char const *devname, bool rw);

/* Build a device_t wrapper around a "/<devname>" FAT mount that's already
 * been registered by someone else (usb_msc.c's usb_msc_init(), for
 * FLASH0/MSC) -- same fatfs_open/_read/... plain-POSIX plumbing as
 * fatfs_create_spi(), just without mounting anything itself. See
 * usb_msc.h for why FLASH0 needs this instead of fatfs_create_spi(). */
device_t *fatfs_wrap_mounted_spi(char const *devname);
