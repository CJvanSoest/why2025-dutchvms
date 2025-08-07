#define STBI_ASSERT(x)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdint.h>
#include <stdlib.h>

// RGB565 color conversion macro (same as yours)
#define RGB565(r, g, b)           ((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F))
#define RGB888_TO_RGB565(r, g, b) RGB565(((r) * 31 + 127) / 255, ((g) * 63 + 127) / 255, ((b) * 31 + 127) / 255)

int render_png_to_framebuffer(
    uint16_t *framebuffer, int fb_width, int fb_height, char const *filename, int dest_x, int dest_y
) {
    int img_width, img_height, channels;

    unsigned char *img_data = stbi_load(filename, &img_width, &img_height, &channels, 0);
    if (!img_data) {
        return -1;
    }

    int start_x = (dest_x < 0) ? -dest_x : 0;
    int start_y = (dest_y < 0) ? -dest_y : 0;
    int end_x   = img_width;
    int end_y   = img_height;

    if (dest_x + end_x > fb_width)
        end_x = fb_width - dest_x;
    if (dest_y + end_y > fb_height)
        end_y = fb_height - dest_y;

    for (int y = start_y; y < end_y; y++) {
        int fb_y = dest_y + y;
        if (fb_y < 0 || fb_y >= fb_height)
            continue;

        uint16_t *fb_row = framebuffer + (fb_y * fb_width);

        for (int x = start_x; x < end_x; x++) {
            int fb_x = dest_x + x;
            if (fb_x < 0 || fb_x >= fb_width)
                continue;

            int pixel_index = (y * img_width + x) * channels;

            uint8_t r = img_data[pixel_index];
            uint8_t g = img_data[pixel_index + 1];
            uint8_t b = img_data[pixel_index + 2];

            if (channels == 4) {
                uint8_t a = img_data[pixel_index + 3];
                if (a < 128)
                    continue; // Skip transparent pixels
            }

            fb_row[fb_x] = RGB888_TO_RGB565(r, g, b);
        }
    }

    stbi_image_free(img_data);
    return 0;
}

int render_png_with_alpha_scaled(
    uint16_t *framebuffer, int fb_width, int fb_height, char const *filename, int dest_x, int dest_y, int scale_factor
) {
    int img_width, img_height, channels;

    unsigned char *img_data = stbi_load(filename, &img_width, &img_height, &channels, 4);
    if (!img_data) {
        return -1;
    }

    int scaled_width  = img_width * scale_factor;
    int scaled_height = img_height * scale_factor;

    int start_x = (dest_x < 0) ? (-dest_x + scale_factor - 1) / scale_factor : 0;
    int start_y = (dest_y < 0) ? (-dest_y + scale_factor - 1) / scale_factor : 0;
    int end_x   = img_width;
    int end_y   = img_height;

    if (dest_x + scaled_width > fb_width) {
        end_x = (fb_width - dest_x + scale_factor - 1) / scale_factor;
    }
    if (dest_y + scaled_height > fb_height) {
        end_y = (fb_height - dest_y + scale_factor - 1) / scale_factor;
    }

    for (int y = start_y; y < end_y; y++) {
        for (int x = start_x; x < end_x; x++) {
            int     pixel_index = (y * img_width + x) * 4;
            uint8_t src_r       = img_data[pixel_index];
            uint8_t src_g       = img_data[pixel_index + 1];
            uint8_t src_b       = img_data[pixel_index + 2];
            uint8_t alpha       = img_data[pixel_index + 3];

            if (alpha == 0)
                continue; // Fully transparent

            uint16_t pixel_color;
            if (alpha == 255) {
                // Fully opaque - no blending needed
                pixel_color = RGB888_TO_RGB565(src_r, src_g, src_b);
            } else {
                pixel_color = RGB888_TO_RGB565(src_r, src_g, src_b);
            }

            for (int dy = 0; dy < scale_factor; dy++) {
                for (int dx = 0; dx < scale_factor; dx++) {
                    int fb_x = dest_x + x * scale_factor + dx;
                    int fb_y = dest_y + y * scale_factor + dy;

                    if (fb_x < 0 || fb_x >= fb_width || fb_y < 0 || fb_y >= fb_height) {
                        continue;
                    }

                    uint16_t *fb_pixel = framebuffer + (fb_y * fb_width + fb_x);

                    if (alpha == 255) {
                        *fb_pixel = pixel_color;
                    } else {
                        uint16_t existing = *fb_pixel;

                        uint8_t dst_r = ((existing >> 11) & 0x1F) << 3;
                        uint8_t dst_g = ((existing >> 5) & 0x3F) << 2;
                        uint8_t dst_b = (existing & 0x1F) << 3;

                        uint8_t final_r = (src_r * alpha + dst_r * (255 - alpha)) / 255;
                        uint8_t final_g = (src_g * alpha + dst_g * (255 - alpha)) / 255;
                        uint8_t final_b = (src_b * alpha + dst_b * (255 - alpha)) / 255;

                        *fb_pixel = RGB888_TO_RGB565(final_r, final_g, final_b);
                    }
                }
            }
        }
    }

    stbi_image_free(img_data);
    return 0;
}

int render_jpg_to_framebuffer(
    uint16_t *framebuffer, int fb_width, int fb_height, char const *filename, int dest_x, int dest_y
) {
    int img_width, img_height, channels;

    unsigned char *img_data = stbi_load(filename, &img_width, &img_height, &channels, 3);
    if (!img_data) {
        return -1;
    }

    int start_x = (dest_x < 0) ? -dest_x : 0;
    int start_y = (dest_y < 0) ? -dest_y : 0;
    int end_x   = img_width;
    int end_y   = img_height;

    if (dest_x + end_x > fb_width)
        end_x = fb_width - dest_x;
    if (dest_y + end_y > fb_height)
        end_y = fb_height - dest_y;

    for (int y = start_y; y < end_y; y++) {
        int fb_y = dest_y + y;
        if (fb_y < 0 || fb_y >= fb_height)
            continue;

        uint16_t *fb_row = framebuffer + (fb_y * fb_width);

        for (int x = start_x; x < end_x; x++) {
            int fb_x = dest_x + x;
            if (fb_x < 0 || fb_x >= fb_width)
                continue;

            int pixel_index = (y * img_width + x) * channels;

            uint8_t r = img_data[pixel_index];
            uint8_t g = img_data[pixel_index + 1];
            uint8_t b = img_data[pixel_index + 2];

            fb_row[fb_x] = RGB888_TO_RGB565(r, g, b);
        }
    }

    stbi_image_free(img_data);
    return 0;
}

int render_png_centered(uint16_t *framebuffer, int fb_width, int fb_height, char const *filename) {
    int img_width, img_height, channels;

    if (!stbi_info(filename, &img_width, &img_height, &channels)) {
        return -1;
    }

    int dest_x = (fb_width - img_width) / 2;
    int dest_y = (fb_height - img_height) / 2;

    return render_png_to_framebuffer(framebuffer, fb_width, fb_height, filename, dest_x, dest_y);
}
