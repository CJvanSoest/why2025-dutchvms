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

/* Minimal app-facing display-brightness API (task #31) — analogous in shape
 * to badgevms/nvs.h: one get + one set, no device handle, since there is
 * only ever one LCD on this hardware.
 *
 * IMPORTANT caveat: schematic research (see the BADGE_BACKLIGHT_GPIO comment
 * in badgevms/drivers/st7703.c) found that the LCD backlight's dimming
 * circuit (an AP3032 boost converter) is driven from a GPIO on the ESP32-C6
 * co-processor, not a locally-confirmed pin on the ESP32-P4 this kernel runs
 * on. Until that gap is closed (e.g. a P4<->C6 remote-GPIO bridge over the
 * existing ESP-Hosted link), bv_display_set_brightness() stores/returns the
 * requested percentage correctly, but may not visibly change the physical
 * backlight. Callers should treat it as "the badge remembers your preferred
 * brightness" rather than "the screen dims today". */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the LCD backlight brightness, 0 (dimmest) - 100 (brightest). Values
 * above 100 are clamped. See the caveat above about current hardware
 * support. */
void bv_display_set_brightness(uint8_t percent);

/* Returns the last value passed to bv_display_set_brightness() (100 =
 * full brightness, the default at boot before any app has called set()). */
uint8_t bv_display_get_brightness(void);

#ifdef __cplusplus
}
#endif
