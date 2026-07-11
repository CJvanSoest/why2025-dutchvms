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

/* Minimal app-facing PSRAM heap-usage query — PSRAM is working memory
 * (framebuffers, heap allocations, per-app memory), not a mountable
 * filesystem like FLASH0/SD0, so this reports byte counts for a UI status
 * bar rather than exposing anything resembling statvfs().
 *
 * Scope note: PSRAM on this badge is split into three separately-managed
 * regions (see badgevms/memory.h) - a small kernel dlmalloc heap, a
 * buddy-allocated framebuffer pool, and per-app PIE-ELF memory. This reports
 * only the kernel heap region (where e.g. bv_nvs_* and general kernel
 * allocations live) via dlmalloc_footprint(), NOT total PSRAM chip-wide -
 * that's the one region with a single well-defined allocator to query
 * cheaply. ESP-IDF's own heap_caps_get_free_size(MALLOC_CAP_SPIRAM) is
 * deliberately NOT used here: this codebase wraps heap_caps_*(MALLOC_CAP_
 * SPIRAM) calls to redirect through dlmalloc (badgevms/memory_heap_caps.c),
 * so the standard ESP-IDF accounting no longer reflects real PSRAM usage.
 * "used" is the arena's current high-water-mark footprint, not a byte-exact
 * live-allocated figure (this build compiles dlmalloc with NO_MALLINFO, so
 * the usual used/free breakdown isn't available) - fine for a long-lived
 * kernel heap, just don't expect it to shrink much after frees. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fills *used_bytes / *total_bytes with the kernel PSRAM heap's current
 * allocated bytes and configured capacity. Always returns true on this
 * platform (the kernel heap is a compile-time-sized region, not optional
 * hardware) - the bool return is for API-shape consistency with the rest of
 * this header family (nvs.h, led_matrix.h, status_led.h, display.h). */
bool bv_psram_get_usage(uint32_t *used_bytes, uint32_t *total_bytes);

#ifdef __cplusplus
}
#endif
