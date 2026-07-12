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

/* Minimal app-facing display-brightness API (task #31, actually dims the
 * screen as of task #38) — analogous in shape to badgevms/nvs.h: one get +
 * one set, no device handle, since there is only ever one LCD on this
 * hardware.
 *
 * The LCD backlight's dimming circuit (an AP3032 boost converter) is driven
 * from a GPIO on the ESP32-C6 co-processor, not a pin on the ESP32-P4 this
 * kernel runs on (confirmed via KiCad schematic, see the BADGE_BACKLIGHT_GPIO
 * comment in badgevms/drivers/st7703.c) — bv_display_set_brightness()
 * forwards the value to the C6 over the existing ESP-Hosted custom_data
 * channel (display_backlight_client.h), fire-and-forget, same shape as the
 * keyboard backlight's own RPC. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the LCD backlight brightness, 0 (dimmest) - 100 (brightest). Values
 * above 100 are clamped. */
void bv_display_set_brightness(uint8_t percent);

/* Returns the last value passed to bv_display_set_brightness() (100 =
 * full brightness, the default at boot before any app has called set()). */
uint8_t bv_display_get_brightness(void);

#ifdef __cplusplus
}
#endif
