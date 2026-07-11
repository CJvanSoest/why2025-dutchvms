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

/* Kernel-internal contract for the PCA9698 LED-matrix add-on (rows PB0-11 x
 * cols PA0-19, monochrome/on-off — NOT the 4x RGBW status LEDs on GPIO7,
 * those are a separate ws2812-style chain, see ws2812_set()/ws2812_show() in
 * badgevms_i2c_bus.c). led_matrix_clear/pixel/row/fill/brightness are
 * defined (non-static) in badgevms_i2c_bus.c; this header just gives
 * led_matrix_bridge.c (the app-facing wrapper, see badgevms/led_matrix.h) a
 * declaration to call them against.
 *
 * bv_mtx_app_control lets an app take exclusive ownership of the shared
 * mtx_fb framebuffer: while true, badgevms_i2c_bus.c's own boot-time demo
 * task (mtx_demo_task, the bouncing-pixel/border animation) stops writing to
 * mtx_fb so the app's own writes aren't clobbered every ~120ms. The
 * multiplex refresh task keeps running regardless — it only reads mtx_fb —
 * so whatever the app last wrote stays visibly lit on the panel. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BV_MTX_ROWS 12
#define BV_MTX_COLS 20

void led_matrix_clear(void);
void led_matrix_pixel(int r, int c, bool on);
void led_matrix_row(int r, uint32_t mask);
void led_matrix_fill(bool on);
void led_matrix_brightness(int pct);

extern volatile bool bv_mtx_app_control;
