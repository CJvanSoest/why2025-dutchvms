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

#include "boot_progress.h"

#include "badgevms/device.h"
#include "badgevms_config.h"
#include "compositor/font.h"
#include "compositor/pixel_functions.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static char const *TAG = "boot_progress";

/* pixel_functions.h's draw_text_rotated() draws the 5x7 font at 1x --
 * fine at app/window scale, unreadably small full-frame on a 720x720
 * panel (confirmed on hardware, issue #96). draw_text_3x() below
 * reimplements it locally at 3x, reusing font_data/FONT_WIDTH/FONT_HEIGHT
 * (font.h) and draw_filled_rect_rotated() (pixel_functions.h) instead of
 * touching the shared, kernel-wide draw_text_rotated() other callers
 * still use at 1x. */
#define BOOT_PROGRESS_SCALE 3
#define BOOT_PROGRESS_BAR_H (FONT_HEIGHT * BOOT_PROGRESS_SCALE + 20)
#define BOOT_PROGRESS_COLOR_BG   0x0000 /* black */
#define BOOT_PROGRESS_COLOR_TEXT 0xFFFF /* white */

/* Real boot work between stages can take well under a second (e.g. when
 * WIFI0 skips the C6 reflash because it's already current) -- without a
 * floor, a message can be overwritten by the next one before it's even
 * readable. Hardware-tested at 400ms (issue #96): the one message that
 * happened to stay up several real seconds (WIFI0 doing an actual C6
 * reflash that run) photographed perfectly readable once the camera
 * settled, while the four ~400ms ones looked "garbled" -- that was motion
 * blur from the transition, not a rendering bug, confirming 400ms just
 * isn't enough dwell time for a camera (or an eye scanning the screen) to
 * resolve it. Bumped to 700ms. This blocks boot by up to this much per
 * call (5 calls, worst case +3.5s total) -- a deliberate legibility/speed
 * tradeoff for a UX feature, not something perf-sensitive code should ever
 * do. */
#define BOOT_PROGRESS_MIN_VISIBLE_MS 700

/* font_data only covers A-Z/0-9/space (see font.h) -- anything else
 * (lowercase is remapped by char_to_font_index(), but '.', '+', etc are
 * not) silently draws as a blank space. Callers should stick to
 * A-Z/0-9/space in boot_progress() messages; this only guards against a
 * garbage character index, not against those gaps. */
static int text_width(char const *text) {
    int len = (int)strlen(text);
    if (len == 0) {
        return 0;
    }
    /* Each glyph is FONT_WIDTH wide plus a 1px gap, except the trailing
     * gap after the last glyph, which is never drawn. */
    return len * (FONT_WIDTH + 1) * BOOT_PROGRESS_SCALE - BOOT_PROGRESS_SCALE;
}

static void draw_text_3x(uint16_t *fb, char const *text, int x, int y, uint16_t color) {
    for (int i = 0; text[i]; i++) {
        int font_idx = char_to_font_index(text[i]);
        for (int row = 0; row < FONT_HEIGHT; row++) {
            unsigned char line = font_data[font_idx][row];
            for (int col = 0; col < FONT_WIDTH; col++) {
                if (line & (0x80 >> col)) {
                    draw_filled_rect_rotated(
                        fb,
                        x + col * BOOT_PROGRESS_SCALE,
                        y + row * BOOT_PROGRESS_SCALE,
                        BOOT_PROGRESS_SCALE,
                        BOOT_PROGRESS_SCALE,
                        color
                    );
                }
            }
        }
        x += (FONT_WIDTH + 1) * BOOT_PROGRESS_SCALE;
    }
}

void boot_progress(char const *msg) {
    lcd_device_t *lcd = (lcd_device_t *)device_get("PANEL0");
    if (!lcd) {
        ESP_LOGW(TAG, "PANEL0 not registered yet, dropping: %s", msg);
        return;
    }

    uint16_t *fb = NULL;
    lcd->_getfb((void *)lcd, 0, (void **)&fb);
    if (!fb) {
        ESP_LOGW(TAG, "PANEL0 has no framebuffer yet, dropping: %s", msg);
        return;
    }

    /* Vertically centered, not a bottom strip -- the bottom strip was hard
     * to read even at 2x (cramped against the panel edge). Horizontally
     * centered on this message's own width, since messages vary in length. */
    int bar_y  = (FRAMEBUFFER_MAX_H - BOOT_PROGRESS_BAR_H) / 2;
    int text_y = bar_y + (BOOT_PROGRESS_BAR_H - FONT_HEIGHT * BOOT_PROGRESS_SCALE) / 2;
    int text_x = (FRAMEBUFFER_MAX_W - text_width(msg)) / 2;

    /* Clear the whole strip first -- msg lengths vary between calls, and
     * we're not tracking the previous line's width to do a narrower clear. */
    draw_filled_rect_rotated(fb, 0, bar_y, FRAMEBUFFER_MAX_W, BOOT_PROGRESS_BAR_H, BOOT_PROGRESS_COLOR_BG);
    draw_text_3x(fb, msg, text_x, text_y, BOOT_PROGRESS_COLOR_TEXT);

    /* Push just the strip we touched, not the whole 720x720 frame -- this
     * is called several times during boot and each call already blocks on
     * the panel DMA (see st7703.c's draw()/esp_lcd_panel_draw_bitmap()), no
     * need to push pixels nothing changed. Safe to call directly like this
     * (bypassing window_present()/the compositor queue entirely): the
     * compositor task's own render path is gated on a non-empty
     * window_stack (see compositor.c), which is guaranteed empty for the
     * entire window boot_progress() is used in -- the first window is
     * created once run_init() (the last thing app_main() calls) spawns the
     * launcher app.
     *
     * _draw()'s 3rd/4th args are END coordinates (esp_lcd_panel_draw_bitmap()
     * takes x_start/y_start/x_end/y_end, exclusive), not width/height --
     * confirmed against components/esp_lcd/include/esp_lcd_panel_ops.h.
     * compositor.c's only call site (0, 0, FRAMEBUFFER_MAX_W,
     * FRAMEBUFFER_MAX_H) can't disambiguate the two conventions since it
     * always draws the full frame from the origin; this strip is not at the
     * origin, so getting this wrong would draw nothing or the wrong rows. */
    lcd->_draw(
        (void *)lcd,
        0,
        bar_y,
        FRAMEBUFFER_MAX_W,
        bar_y + BOOT_PROGRESS_BAR_H,
        fb + (size_t)bar_y * FRAMEBUFFER_MAX_W
    );

    vTaskDelay(pdMS_TO_TICKS(BOOT_PROGRESS_MIN_VISIBLE_MS));
}
