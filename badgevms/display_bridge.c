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

/* See badgevms/display.h. Thin wrapper around st7703_set_brightness/
 * st7703_get_brightness (drivers/st7703.h) — same pattern as
 * led_matrix_bridge.c wrapping the PCA9698 primitives, or nvs_bridge.c
 * wrapping nvs_get_blob/nvs_set_blob. */

#include "badgevms/display.h"
#include "drivers/st7703.h"

void bv_display_set_brightness(uint8_t percent) {
    st7703_set_brightness(percent);
}

uint8_t bv_display_get_brightness(void) {
    return st7703_get_brightness();
}
