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

/* Minimal app-facing API for the PCA9698 LED-matrix add-on — analogous in
 * shape to badgevms/nvs.h. This is the 12x20 monochrome (on/off, no color)
 * dot-matrix panel wired to the PCA9698 GPIO expander at I2C addr 0x20
 * (SDA=GPIO22/SCL=GPIO9, RESET=GPIO5, OE=GPIO8), NOT the 4x RGBW status LEDs
 * on GPIO7 (those stay firmware-only — LoRa/WiFi/notify status indicators,
 * see ws2812_task() in drivers/badgevms_i2c_bus.c, no app API).
 *
 * The matrix panel is a single shared hardware resource: the firmware runs
 * its own boot-time demo animation (a bouncing pixel inside a border) on it
 * by default. Call bv_led_matrix_take() before drawing anything so the demo
 * stops touching the shared framebuffer, and bv_led_matrix_release() when
 * your app is done (e.g. right before exiting) so the demo resumes for the
 * next app / idle badge. If you don't release, the panel simply keeps
 * showing your app's last frame until something else calls take() again —
 * harmless, just not restoring the default animation.
 *
 * Rows/cols are addressed (0,0)..(BV_LED_MATRIX_ROWS-1,BV_LED_MATRIX_COLS-1).
 * There is no color/brightness per pixel — only on/off; bv_led_matrix_set_
 * brightness() is a single global PWM dimmer for the whole panel (via the
 * PCA9698's active-low OE pin), not per-LED. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BV_LED_MATRIX_ROWS 12
#define BV_LED_MATRIX_COLS 20

/* Claim/release exclusive control of the matrix framebuffer, see above. Both
 * are cheap flag flips — safe to call every frame if convenient. */
void bv_led_matrix_take(void);
void bv_led_matrix_release(void);

/* Turns every pixel off (does not implicitly take/release control). */
void bv_led_matrix_clear(void);

/* Sets a single pixel. Out-of-range row/col is silently ignored. */
void bv_led_matrix_set_pixel(int row, int col, bool on);

/* Sets an entire row at once; bit c of col_mask (0..BV_LED_MATRIX_COLS-1) is
 * pixel (row, c). Out-of-range row is silently ignored. */
void bv_led_matrix_set_row(int row, uint32_t col_mask);

/* Turns every pixel on (true) or off (false). */
void bv_led_matrix_fill(bool on);

/* Global panel brightness, 0-100 (clamped). Affects the whole matrix, not
 * per-pixel. Firmware default is 60. */
void bv_led_matrix_set_brightness(int pct);

#ifdef __cplusplus
}
#endif
