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

/* Kernel-side boot-progress text, GitHub issue #96: app_main() now brings
 * PANEL0 (badgevms/why2025_firmware.c) up before FLASH0/SD0/WIFI0, so there's
 * a real panel to draw on during the remaining boot steps -- this draws a
 * single-line status string directly into PANEL0's framebuffer and pushes it
 * to the physical panel, bypassing the window/compositor pipeline entirely
 * (no window exists yet -- the first one is created once run_init(), the
 * last thing app_main() calls, spawns the launcher app). Safe to call
 * concurrently with the compositor task: the compositor's own render path is
 * gated on a non-empty window_stack (see compositor.c), which is guaranteed
 * empty for this whole window.
 *
 * No-op (logs and returns) if PANEL0 isn't registered yet -- callable from
 * anywhere in app_main() after the PANEL0/KEYBOARD0/compositor_init() block,
 * not just the exact original call sites. */
void boot_progress(char const *msg);
