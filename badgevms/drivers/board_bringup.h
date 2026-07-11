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

/* Kicks off the LED-matrix add-on bring-up sequence: default the vibration
 * motor (GPIO3) off, probe I2C2 (GPIO22/9 or GPIO9/22) for a PCA9698, and on
 * success start the matrix refresh task (see led_matrix_pca9698.c) and the
 * WS2812 status-LED task (see status_led_ws2812.c). Runs as its own
 * background task; safe to call unconditionally even if no add-on board is
 * attached. Called once from badgevms_i2c_bus_create() when the main I2C bus
 * (port 0) is initialized. */
void board_bringup_start(void);
