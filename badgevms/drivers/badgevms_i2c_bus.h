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
#include "i2c_bus.h"

device_t *badgevms_i2c_bus_create(char const *name, uint8_t port, uint32_t clk_speed);

// Shared main-bus (I2C_NUM_0, GPIO18/20) bring-up for drivers that need a raw
// i2c_bus_handle_t directly (not through the device_t/VFS layer above) --
// used by bosch_bmi270.c and bosch_bme690.c, which were previously each
// carrying their own copy of this same i2c_config_t + i2c_bus_create() call.
// Returns NULL on failure; caller logs with its own TAG.
i2c_bus_handle_t why_i2c0_main_bus_create(void);
