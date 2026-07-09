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

#include <stdbool.h>
#include <stdint.h>

/* Cross-app notification counters, keyed by an app's manifest
 * unique_identifier (the same string used by application_launch() /
 * task_set_application_uid()). Lets a background app (e.g. cj_meshcore
 * receiving a message while minimized) signal the launcher without either
 * side needing to know about the other. One small shared table serves the
 * status-bar counters, the blinking taskbar icon, and (later) LED-notify -
 * all three read the same state instead of each needing their own channel.
 *
 * In-memory only (not persisted across reboots) - notifications are
 * transient by nature and this avoids NVS flash-write wear on bursty
 * message traffic. */

void notify_system_init(void); /* kernel-only, called once from app_main() - not exported to apps */

void     notify_increment(char const *unique_identifier);
void     notify_clear(char const *unique_identifier);
uint32_t notify_get_count(char const *unique_identifier);
bool     notify_get_dirty(char const *unique_identifier);

/* Kernel-only (not exported to apps via symbols.yml): true if ANY tracked app
 * currently has a dirty/unread notification. Used by the LED-matrix add-on's
 * ws2812_task (badgevms_i2c_bus.c) to drive a single shared "unread" indicator
 * on the RGBW side LEDs without needing to know which app(s) are dirty. */
bool notify_any_dirty(void);
