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

/* See badgevms/status_led.h. Thin wrapper around the internal
 * status_led_set/show/clear/set_brightness primitives already implemented
 * in drivers/badgevms_i2c_bus.c (same pattern as led_matrix_bridge.c
 * wrapping led_matrix_clear/pixel/... for the matrix), plus the
 * take()/release() arbitration flag so an app can own the shared 4-LED
 * chain without the firmware's own ws2812_task status indicator fighting
 * over it. */

#include "badgevms/status_led.h"
#include "drivers/status_led_internal.h"

void bv_status_led_take(void) {
    bv_led_app_control = true;
}

void bv_status_led_release(void) {
    bv_led_app_control = false;
}

void bv_status_led_set(int index, uint8_t r, uint8_t g, uint8_t b) {
    status_led_set(index, r, g, b);
}

void bv_status_led_show(void) {
    status_led_show();
}

void bv_status_led_clear(void) {
    status_led_clear();
}

void bv_status_led_set_brightness(int pct) {
    status_led_set_brightness(pct);
}
