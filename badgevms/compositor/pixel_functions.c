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
/* Sample one pixel of `src` (row width src_w) at (x,y) and convert it to
 * this compositor's RGB565 output format. Only the 6 pixel_format_t values
 * framebuffer_allocate() ever actually hands out reach here (everything
 * else is coerced to RGB565 at allocation time).
 *
 * RGB565/BGR565 mirror the exact net R/B transform the old PPA path's
 * rgb_swap flag produced for these two formats -- the only two any shipped
 * app currently uses -- to preserve known (mostly correct; the narrow
 * value-dependent bug this rewrite exists to fix aside) color behavior
 * rather than risk a much worse blind regression by guessing at a "fix"
 * with no hardware to verify it on. The panel wants BGR-ordered RGB565
 * (st7703.c sets LCD_RGB_ELEMENT_ORDER_BGR) -- that swap still has to
 * happen somewhere; this is the one and only place it happens now, instead
 * of composing with a second swap at the panel/MADCTL level the way the
 * old PPA rgb_swap did.
 *
 * The four *8888 formats use SDL3's own bit-layout convention directly
 * (first-named channel = bits 31-24 of a native uint32 load) rather than
 * mirroring the old PPA rgb_swap/mode grouping, which -- on inspection --
 * doesn't self-consistently distinguish all four of them. Doesn't matter in
 * practice today (no shipped app uses any of them), but NOT
 * hardware-verified -- test with a real app before relying on it. */
__attribute__((always_inline)) static inline uint16_t
    sample_pixel_rgb565(void const *src, int src_w, int x, int y, pixel_format_t format) {
    switch (format) {
        case BADGEVMS_PIXELFORMAT_BGR565: return ((uint16_t const *)src)[(size_t)y * src_w + x];

        case BADGEVMS_PIXELFORMAT_ARGB8888: {
            uint32_t val = ((uint32_t const *)src)[(size_t)y * src_w + x];
            return rgb888_to_rgb565((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
        }
        case BADGEVMS_PIXELFORMAT_RGBA8888: {
            uint32_t val = ((uint32_t const *)src)[(size_t)y * src_w + x];
            return rgb888_to_rgb565((val >> 24) & 0xFF, (val >> 16) & 0xFF, (val >> 8) & 0xFF);
        }
        case BADGEVMS_PIXELFORMAT_ABGR8888: {
            uint32_t val = ((uint32_t const *)src)[(size_t)y * src_w + x];
            return rgb888_to_rgb565(val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF);
        }
        case BADGEVMS_PIXELFORMAT_BGRA8888: {
            uint32_t val = ((uint32_t const *)src)[(size_t)y * src_w + x];
            return rgb888_to_rgb565((val >> 8) & 0xFF, (val >> 16) & 0xFF, (val >> 24) & 0xFF);
        }
        case BADGEVMS_PIXELFORMAT_RGB565:
        default: {
            uint16_t px = ((uint16_t const *)src)[(size_t)y * src_w + x];
            uint16_t r  = (px >> 11) & 0x1F;
            uint16_t g  = (px >> 5) & 0x3F;
            uint16_t b  = px & 0x1F;
            return (uint16_t)((b << 11) | (g << 5) | r);
        }
    }
}

IRAM_ATTR void blit_rect_rotated(
    uint16_t        *dst,
    void const      *src,
    int              src_w,
    pixel_format_t   src_format,
    window_rect_t    src_rect,
    window_rect_t    dst_rect,
    float            scale,
    rotation_angle_t rotation
) {
    if (dst_rect.w <= 0 || dst_rect.h <= 0 || src_rect.w <= 0 || src_rect.h <= 0)
        return;

    float inv_scale = 1.0f / scale;
    int   src_max_x = src_rect.x + src_rect.w - 1;
    int   src_max_y = src_rect.y + src_rect.h - 1;

    for (int dy = 0; dy < dst_rect.h; dy++) {
        int sy = src_rect.y + (int)((float)dy * inv_scale);
        if (sy > src_max_y)
            sy = src_max_y;

        for (int dx = 0; dx < dst_rect.w; dx++) {
            int sx = src_rect.x + (int)((float)dx * inv_scale);
            if (sx > src_max_x)
                sx = src_max_x;

            uint16_t color = sample_pixel_rgb565(src, src_w, sx, sy, src_format);

            int fb_x = 0, fb_y = 0;
            rotate_coordinates(dst_rect.x + dx, dst_rect.y + dy, rotation, &fb_x, &fb_y);
            if (fb_x >= 0 && fb_x < FRAMEBUFFER_MAX_W && fb_y >= 0 && fb_y < FRAMEBUFFER_MAX_H) {
                dst[fb_y * FRAMEBUFFER_MAX_W + fb_x] = color;
            }
        }
    }
}
