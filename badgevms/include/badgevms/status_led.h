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

/* Minimal app-facing API for the 4x RGBW status LEDs (SK6812/XL-5050RGBWC-
 * style, WS-DATA = GPIO7) - analogous in shape to badgevms/led_matrix.h.
 * This is the 4-LED chain the firmware itself uses for LoRa/WiFi/notify
 * status (see ws2812_task() in drivers/badgevms_i2c_bus.c), NOT the 12x20
 * PCA9698 dot-matrix panel (that's badgevms/led_matrix.h).
 *
 * The LED chain is a single shared hardware resource: by default the
 * firmware's own ws2812_task drives it with radio/wifi/notify status every
 * ~1s. Call bv_status_led_take() before writing anything so that status
 * loop stops touching the shared LED buffer, and bv_status_led_release()
 * when your app is done (e.g. right before exiting) so real status resumes
 * on the next ~1s tick. If you don't release, the chain simply keeps
 * showing your app's last frame until something else calls take() again -
 * harmless, just not restoring the status indicators.
 *
 * LEDs are addressed 0..BV_STATUS_LED_COUNT-1 (this is the same physical
 * order/index the firmware uses: 0=LoRa, 1=WiFi, 2=DM notify, 3=channel
 * notify when firmware-owned - once you take() them those meanings no
 * longer apply, they're just 4 LEDs to draw on). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BV_STATUS_LED_COUNT 4

/* Claim/release exclusive control of the 4 status LEDs, see above. Both are
 * cheap flag flips - safe to call every frame if convenient. */
void bv_status_led_take(void);
void bv_status_led_release(void);

/* Sets one LED (0..BV_STATUS_LED_COUNT-1) to a fixed RGB color (0-255 each,
 * W is always 0 - matches the firmware's own use of this chain, see the
 * XL-5050RGBWC W-channel note in badgevms_i2c_bus.c). Out-of-range index is
 * silently ignored. Buffered - call bv_status_led_show() to push to
 * hardware. Brightness-scaled by bv_status_led_set_brightness(). */
void bv_status_led_set(int index, uint8_t r, uint8_t g, uint8_t b);

/* Pushes the buffered colors (from bv_status_led_set()) out to the physical
 * LED chain. */
void bv_status_led_show(void);

/* Turns all 4 LEDs off in the buffer (does not implicitly show() or
 * take()/release()). */
void bv_status_led_clear(void);

/* Brightness for app-driven colors, 0-100 (clamped), applied as a percentage
 * scale to whatever bv_status_led_set() is called with next. Independent of
 * the firmware's own fixed status-indicator brightness. Persists across
 * take()/release() (same as bv_led_matrix_set_brightness() for the matrix -
 * it's a global setting, not reset on release). */
void bv_status_led_set_brightness(int pct);

#ifdef __cplusplus
}
#endif
