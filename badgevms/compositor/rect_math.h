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

/* Damage-rectangle maths for the compositor. Deliberately free of ESP-IDF and
 * FreeRTOS headers so host_tests/ can exercise it: a wrong rectangle here does
 * not crash, it leaves stale pixels on the panel, which is expensive to chase
 * on hardware and cheap to pin down here.
 */

#pragma once

#include "badgevms/compositor.h"

#include <stdbool.h>

#define MAX_VISIBLE_RECTS 64

typedef struct {
    window_rect_t rects[MAX_VISIBLE_RECTS];
    int           count;
} rect_array_t;

typedef struct {
    window_rect_t rects[4];
    int           count;
} small_rect_array_t;

__attribute__((always_inline)) inline static bool rect_intersects(window_rect_t a, window_rect_t b) {
    return (a.x < b.x + b.w) && (a.x + a.w > b.x) && (a.y < b.y + b.h) && (a.y + a.h > b.y);
}

__attribute__((always_inline)) inline static window_rect_t rect_intersection(window_rect_t a, window_rect_t b) {
    int left   = (a.x > b.x) ? a.x : b.x;
    int top    = (a.y > b.y) ? a.y : b.y;
    int right  = ((a.x + a.w) < (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int bottom = ((a.y + a.h) < (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);

    return (window_rect_t
    ){.x = left, .y = top, .w = (right > left) ? (right - left) : 0, .h = (bottom > top) ? (bottom - top) : 0};
}

// `a` minus `b`, as up to four non-overlapping pieces.
small_rect_array_t rect_subtract(window_rect_t a, window_rect_t b);

// Coalesce edge-adjacent rectangles of equal extent. Shrinks `arr` in place.
void merge_rectangles(rect_array_t *arr);

// The PPA hardware mishandles strips whose framebuffer height is N*32+1.
bool is_problematic_block_height(int content_height, float scale);

// Split any rectangle of a problematic height in two. Returns true when it
// split something, so the caller can run it again until nothing changes.
// Rectangles that no longer fit within MAX_VISIBLE_RECTS are dropped; the count
// of dropped rectangles is added to *dropped when it is non-NULL, because a
// dropped rectangle is a region that will not be repainted.
bool ppa_workaround_split_rects(rect_array_t *visible, float scale, int *dropped);
