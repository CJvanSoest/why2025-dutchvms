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

/* See badgevms/led_matrix.h. Thin wrapper around the internal
 * led_matrix_clear/pixel/row/fill/brightness primitives already implemented
 * in drivers/badgevms_i2c_bus.c (same pattern as nvs_bridge.c wrapping
 * nvs_get_blob/nvs_set_blob), plus the take()/release() arbitration flag so
 * an app can own the shared matrix framebuffer without the firmware's own
 * boot-demo animation fighting over it. */

#include "badgevms/led_matrix.h"
#include "drivers/led_matrix_internal.h"

void bv_led_matrix_take(void) {
    bv_mtx_app_control = true;
}

void bv_led_matrix_release(void) {
    bv_mtx_app_control = false;
}

void bv_led_matrix_clear(void) {
    led_matrix_clear();
}

void bv_led_matrix_set_pixel(int row, int col, bool on) {
    led_matrix_pixel(row, col, on);
}

void bv_led_matrix_set_row(int row, uint32_t col_mask) {
    led_matrix_row(row, col_mask);
}

void bv_led_matrix_fill(bool on) {
    led_matrix_fill(on);
}

void bv_led_matrix_set_brightness(int pct) {
    led_matrix_brightness(pct);
}
