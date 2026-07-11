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

/* See badgevms/meminfo.h for the scope note on why this reports the kernel
 * dlmalloc heap specifically, not total chip-wide PSRAM.
 *
 * This build compiles dlmalloc with NO_MALLINFO (see APP_COMPILE_FLAGS /
 * -DNO_MALLINFO), so dlmallinfo()/struct mallinfo aren't available - use
 * dlmalloc_footprint() instead, which isn't gated behind that macro. It
 * reports bytes currently obtained from MORECORE (the arena's current high
 * -water mark, not "live allocated minus freed") - dlmalloc doesn't shrink
 * the arena on scattered interior frees, only front-heap frees; for this
 * kernel heap (long-lived allocations - NVS, kernel structures - not a
 * churny alloc/free workload) that's a reasonable "used" proxy in practice,
 * just not a byte-exact one. */

#include "badgevms/meminfo.h"
#include "memory.h"
#include "thirdparty/dlmalloc.h"

bool bv_psram_get_usage(uint32_t *used_bytes, uint32_t *total_bytes) {
    if (used_bytes)
        *used_bytes = (uint32_t)dlmalloc_footprint();
    if (total_bytes)
        *total_bytes = (uint32_t)KERNEL_HEAP_SIZE;
    return true;
}
