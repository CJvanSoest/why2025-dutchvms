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

/* Kernel-internal contract for the 4x RGBW status LEDs (SK6812/XL-5050RGBWC
 * on WS-DATA = GPIO7 - NOT the 12x20 PCA9698 matrix, see
 * drivers/led_matrix_internal.h for that one). status_led_set/show/clear/
 * set_brightness are defined (non-static) in badgevms_i2c_bus.c; this header
 * just gives status_led_bridge.c (the app-facing wrapper, see
 * badgevms/status_led.h) a declaration to call them against.
 *
 * bv_led_app_control lets an app take exclusive ownership of the shared
 * ws_grbw chain: while true, badgevms_i2c_bus.c's own ws2812_task (which
 * otherwise drives LED0-3 off LoRa/WiFi/notify status every ~1s) skips both
 * its own status computation and its own ws2812_show() call, so the app's
 * last-pushed frame stays exactly as drawn. On release, the task's next tick
 * recomputes and redraws real status - same take()/release() shape as
 * bv_mtx_app_control for the LED matrix. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BV_STATUS_LED_COUNT 4

void status_led_set(int i, uint8_t r, uint8_t g, uint8_t b);
void status_led_show(void);
void status_led_clear(void);
void status_led_set_brightness(int pct);

extern volatile bool bv_led_app_control;
