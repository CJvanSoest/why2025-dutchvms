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

/* Kernel-internal RPC sender for the display backlight (task #38). Not part
 * of the app-facing SDK surface (that's badgevms/display.h's
 * bv_display_set_brightness(), implemented in display_bridge.c ->
 * st7703_set_brightness()) -- this is the transport st7703.c calls into,
 * same split as lora_proto_client.h being internal to drivers/ while
 * badgevms/lora.h is the public API. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sends the backlight percent (0-100, already clamped by the caller) to the
 * C6's display-backlight PWM channel over the esp-hosted custom_data
 * channel. Fire-and-forget -- returns immediately, doesn't confirm the C6
 * actually received/applied it. */
void bv_display_backlight_send(uint8_t percent);

#ifdef __cplusplus
}
#endif
