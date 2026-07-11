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

/* App-facing API for the keyboard backlight (8x LTW-010DCG LEDs driven by
 * an AP3032 boost converter, src/hardware/Carrier/keyboard.kicad_sch) --
 * why2025-apps#1 hardware-test feedback.
 *
 * Unlike the LCD backlight (badgevms/display.h, whose brightness setter
 * currently only stores a preference -- see its own doc comment on the
 * still-open P4<->C6 GPIO gap), the keyboard backlight's PWM channel lives
 * entirely on the ESP32-C6 (LEDC_CHANNEL_1, initialized in
 * connectivity_esp_hosted/slave/main/tanmatsu/tanmatsu_main.c) and this
 * API actually reaches it: bv_keyboard_set_backlight() sends a one-way
 * esp-hosted custom_data message (TANMATSU_EVENT_KEYBOARD_BL,
 * priv_events.h) to the C6, which applies it directly via ledc_set_duty()/
 * ledc_update_duty() on the already-configured channel. No response is
 * expected or waited for -- same fire-and-forget shape as the LoRa
 * client's non-request calls, just simpler (a single byte, no
 * request/reply framing needed). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the keyboard backlight brightness, 0 (off) - 100 (brightest).
 * Values above 100 are clamped. Fire-and-forget over the esp-hosted
 * custom_data channel to the C6 -- returns immediately, doesn't confirm
 * the C6 actually received/applied it (same tolerance esp_hosted_send_
 * custom_data() callers elsewhere in this kernel already have). */
void bv_keyboard_set_backlight(uint8_t percent);

#ifdef __cplusplus
}
#endif
