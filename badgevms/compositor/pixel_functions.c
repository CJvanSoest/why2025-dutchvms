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

#include "pixel_functions.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "font.h"

#define TAG "pixel_functions"

extern rotation_angle_t rotation;

#if 0
IRAM_ATTR void draw_pixel_rotated(uint16_t *fb, int x, int y, uint16_t color) {
    int fb_x = 0, fb_y = 0;
    rotate_coordinates(x, y, rotation, &fb_x, &fb_y);
    if (fb_x >= 0 && fb_x < FRAMEBUFFER_MAX_W && fb_y >= 0 && fb_y < FRAMEBUFFER_MAX_H) {
        fb[fb_y * FRAMEBUFFER_MAX_W + fb_x] = color;
    }
}
#endif

IRAM_ATTR void draw_pixel_rotated(uint16_t *fb, int x, int y, uint16_t color) {
    int fb_x = 0, fb_y = 0;
    rotate_coordinates(x, y, rotation, &fb_x, &fb_y);

    if (fb_x >= 0 && fb_x < FRAMEBUFFER_MAX_W && fb_y >= 0 && fb_y < FRAMEBUFFER_MAX_H) {
        fb[fb_y * FRAMEBUFFER_MAX_W + fb_x] = color;
    } else {
        ESP_LOGV(TAG, "Out of bounds draw: (%d,%d) -> fb(%d,%d)", x, y, fb_x, fb_y);
    }
}

IRAM_ATTR void draw_filled_rect_rotated(uint16_t *fb, int x, int y, int width, int height, uint16_t color) {
    if (width <= 0 || height <= 0)
        return;

    for (int py = y; py < y + height; py++) {
        for (int px = x; px < x + width; px++) {
            draw_pixel_rotated(fb, px, py, color);
        }
    }
}

IRAM_ATTR void draw_rect_rotated(uint16_t *fb, int x, int y, int width, int height, uint16_t color) {
    if (width <= 0 || height <= 0)
        return;

    // Top edge
    for (int px = x; px < x + width; px++) {
        draw_pixel_rotated(fb, px, y, color);
    }

    // Bottom edge
    if (height > 1) {
        for (int px = x; px < x + width; px++) {
            draw_pixel_rotated(fb, px, y + height - 1, color);
        }
    }

    // Left and right edges
    if (height > 2) {
        for (int py = y + 1; py < y + height - 1; py++) {
            draw_pixel_rotated(fb, x, py, color);
            if (width > 1) {
                draw_pixel_rotated(fb, x + width - 1, py, color);
            }
        }
    }
}

IRAM_ATTR int char_to_font_index(char c) {
    if (c == ' ')
        return 0;
    if (c >= 'A' && c <= 'Z')
        return 1 + (c - 'A');
    if (c >= 'a' && c <= 'z')
        return 1 + (c - 'a'); // Map lowercase to uppercase
    if (c >= '0' && c <= '9')
        return 27 + (c - '0');
    return 0; // Default to space
}

IRAM_ATTR void draw_char_rotated(uint16_t *fb, char c, int x, int y, uint16_t color) {
    int font_idx = char_to_font_index(c);

    for (int row = 0; row < FONT_HEIGHT; row++) {
        unsigned char line = font_data[font_idx][row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (line & (0x80 >> col)) {
                draw_pixel_rotated(fb, x + col, y + row, color);
            }
        }
    }
}

IRAM_ATTR void draw_text_rotated(uint16_t *fb, char const *text, int x, int y, uint16_t color) {
    int len = strlen(text);
    for (int i = 0; i < len; i++) {
        draw_char_rotated(fb, text[i], x + i * (FONT_WIDTH + 1), y, color);
    }
}

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
                        b->x  = b->x;
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
        result.rects[result.count++] = (window_rect_t){.x = overlap.x,
                                                       .y = overlap.y + overlap.h,
                                                       .w = overlap.w,
                                                       .h = (a.y + a.h) - (overlap.y + overlap.h)};
    }

    return result;
}
