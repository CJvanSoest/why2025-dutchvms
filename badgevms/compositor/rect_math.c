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


#include "rect_math.h"

#include <string.h>

void merge_rectangles(rect_array_t *arr) {
    bool merged_any;

    do {
        merged_any = false;

        // Try horizontal merging
        for (int i = 0; i < arr->count && !merged_any; i++) {
            for (int j = i + 1; j < arr->count; j++) {
                window_rect_t *a = &arr->rects[i];
                window_rect_t *b = &arr->rects[j];

                // Can merge horizontally?
                if (a->y == b->y && a->h == b->h) {
                    if (a->x + a->w == b->x) {
                        a->w += b->w;
                        memmove(&arr->rects[j], &arr->rects[j + 1], (arr->count - j - 1) * sizeof(window_rect_t));
                        arr->count--;
                        merged_any = true;
                        break;
                    } else if (b->x + b->w == a->x) {
                        b->w += a->w;
                        memmove(&arr->rects[i], &arr->rects[i + 1], (arr->count - i - 1) * sizeof(window_rect_t));
                        arr->count--;
                        merged_any = true;
                        break;
                    }
                }
            }
        }

        // Try vertical merging
        if (!merged_any) {
            for (int i = 0; i < arr->count && !merged_any; i++) {
                for (int j = i + 1; j < arr->count; j++) {
                    window_rect_t *a = &arr->rects[i];
                    window_rect_t *b = &arr->rects[j];

                    // Can merge vertically?
                    if (a->x == b->x && a->w == b->w) {
                        if (a->y + a->h == b->y) {
                            a->h += b->h;
                            memmove(&arr->rects[j], &arr->rects[j + 1], (arr->count - j - 1) * sizeof(window_rect_t));
                            arr->count--;
                            merged_any = true;
                            break;
                        } else if (b->y + b->h == a->y) {
                            b->h += a->h;
                            memmove(&arr->rects[i], &arr->rects[i + 1], (arr->count - i - 1) * sizeof(window_rect_t));
                            arr->count--;
                            merged_any = true;
                            break;
                        }
                    }
                }
            }
        }
    } while (merged_any);
}

small_rect_array_t rect_subtract(window_rect_t a, window_rect_t b) {
    small_rect_array_t result = {0};

    // No overlap
    if (!rect_intersects(a, b)) {
        result.rects[0] = a;
        result.count    = 1;
        return result;
    }

    window_rect_t overlap = rect_intersection(a, b);

    // Completely covered
    if (overlap.x == a.x && overlap.y == a.y && overlap.w == a.w && overlap.h == a.h) {
        return result;
    }

    // The "degenerate" case is one window in the middle of another
    // this creates a "border" from this window, with a maximum of 4
    // pieces.

    // Left
    if (overlap.x > a.x) {
        result.rects[result.count++] = (window_rect_t){.x = a.x, .y = a.y, .w = overlap.x - a.x, .h = a.h};
    }

    // Right
    if (overlap.x + overlap.w < a.x + a.w) {
        result.rects[result.count++] =
            (window_rect_t){.x = overlap.x + overlap.w, .y = a.y, .w = (a.x + a.w) - (overlap.x + overlap.w), .h = a.h};
    }

    // Top
    if (overlap.y > a.y) {
        result.rects[result.count++] = (window_rect_t){.x = overlap.x, .y = a.y, .w = overlap.w, .h = overlap.y - a.y};
    }

    // Bottom
    if (overlap.y + overlap.h < a.y + a.h) {
        result.rects[result.count++] = (window_rect_t
        ){.x = overlap.x, .y = overlap.y + overlap.h, .w = overlap.w, .h = (a.y + a.h) - (overlap.y + overlap.h)};
    }

    return result;
}

bool is_problematic_block_height(int content_height, float scale) {
    int fb_height = (int)(content_height / scale);
    return fb_height > 32 && (fb_height % 32) == 1;
}

bool ppa_workaround_split_rects(rect_array_t *visible, float scale, int *dropped) {
    rect_array_t new_visible = {0};
    bool         split       = false;
    int          lost        = 0;

    for (int i = 0; i < visible->count; i++) {
        window_rect_t rect = visible->rects[i];

        if (is_problematic_block_height(rect.h, scale)) {
            split           = true;
            int first_half  = (rect.h / 2) - 1;
            int second_half = rect.h - first_half;

            /* Both halves are bounds-checked. The first one used to be written
             * unconditionally, which at a full array wrote rects[64] -- exactly
             * over `count`, then incremented that clobbered value. */
            if (new_visible.count < MAX_VISIBLE_RECTS) {
                new_visible.rects[new_visible.count++] =
                    (window_rect_t){.x = rect.x, .y = rect.y, .w = rect.w, .h = first_half};
            } else {
                lost++;
            }

            if (new_visible.count < MAX_VISIBLE_RECTS) {
                new_visible.rects[new_visible.count++] =
                    (window_rect_t){.x = rect.x, .y = rect.y + first_half, .w = rect.w, .h = second_half};
            } else {
                lost++;
            }
        } else {
            if (new_visible.count < MAX_VISIBLE_RECTS) {
                new_visible.rects[new_visible.count++] = rect;
            } else {
                lost++;
            }
        }
    }

    *visible = new_visible;
    if (dropped) {
        *dropped += lost;
    }
    return split;
}
