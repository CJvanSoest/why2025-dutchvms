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
#define BOOT_PROGRESS_SCALE      3
#define BOOT_PROGRESS_COLOR_TEXT 0xFFFF /* white */

/* v1: a single line, cleared and redrawn on every call, held up by a fixed
 * vTaskDelay() so it stayed on screen long enough to read. Hardware
 * feedback (issue #96) at both 400ms and 700ms: still "unreadable" for
 * every message except the last ("STARTING DUTCHVMS") -- which was never
 * about the delay being too short. That message is simply followed by
 * slower, non-boot_progress()-gated work (app scanning, ELF loading,
 * compositor setup) before anything overwrites it, so it naturally stays
 * up far longer than any of the tuned delays; the other four are cleared
 * the moment the NEXT call lands, however long that took. No fixed delay
 * value fixes that -- it only trades boot latency for a slightly longer
 * chance to catch a message before it's erased.
 *
 * v2 (this version): stop erasing. Each call appends a new line below the
 * previous ones instead of clearing and overwriting the same strip -- a
 * boot log, not a status line. A line's real dwell time becomes "until
 * boot finishes" (several seconds, whatever the actual work takes) rather
 * than a hand-tuned constant, so legibility no longer depends on getting
 * that constant right. BOOT_LOG_MIN_STEP_MS below is now purely cosmetic
 * pacing (so 5 lines landing within the same 50ms on a fast boot don't
 * all slam onto screen in one frame), not a legibility mechanism -- safe
 * to trim or drop entirely without making anything harder to read. */
#define BOOT_LOG_MAX_LINES   8
#define BOOT_LOG_LEFT_X      40
#define BOOT_LOG_LINE_H      (FONT_HEIGHT * BOOT_PROGRESS_SCALE + 14)
#define BOOT_LOG_TOP_Y       ((FRAMEBUFFER_MAX_H - BOOT_LOG_MAX_LINES * BOOT_LOG_LINE_H) / 2)
#define BOOT_LOG_MIN_STEP_MS 150

static int boot_log_next_line = 0;

/* font_data only covers A-Z/0-9/space (see font.h) -- anything else
 * (lowercase is remapped by char_to_font_index(), but '.', '+', etc are
 * not) silently draws as a blank space. Callers should stick to
 * A-Z/0-9/space in boot_progress() messages; this only guards against a
 * garbage character index, not against those gaps. */
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

    if (boot_log_next_line >= BOOT_LOG_MAX_LINES) {
        // More boot_progress() calls than the log has room for -- drop
        // silently rather than overflow into whatever's below the block.
        // Callers are a short, fixed list in why2025_firmware.c today; if
        // that ever grows past BOOT_LOG_MAX_LINES, raise the constant.
        ESP_LOGW(TAG, "boot log full, dropping: %s", msg);
        return;
    }

    if (boot_log_next_line == 0) {
        /* First call ever: PANEL0 was just registered moments ago (see
         * why2025_firmware.c) and has never had a real frame pushed to it.
         * Every other _draw() call in this codebase -- compositor.c's own,
         * every one of them -- pushes the FULL 720x720 frame; this file is
         * the only caller that ever pushes a narrow strip, and doing that
         * as the panel's very first-ever draw left the rest of the
         * framebuffer's memory (never written, whatever the allocator or a
         * previous boot left there) on screen alongside the text, and the
         * panel's own internal state with no full-frame refresh to settle
         * against -- hardware feedback described this as looking like
         * "the screen/rendering hasn't fully started yet". A one-time
         * full-frame black clear+push here, before the first line, is
         * cheap (runs once) and gives the panel exactly the same kind of
         * first draw every other caller already relies on. */
        memset(fb, 0, (size_t)FRAMEBUFFER_MAX_W * FRAMEBUFFER_MAX_H * sizeof(uint16_t));
        lcd->_draw((void *)lcd, 0, 0, FRAMEBUFFER_MAX_W, FRAMEBUFFER_MAX_H, fb);
    }

    int line_y = BOOT_LOG_TOP_Y + boot_log_next_line * BOOT_LOG_LINE_H;
    boot_log_next_line++;

    draw_text_3x(fb, msg, BOOT_LOG_LEFT_X, line_y, BOOT_PROGRESS_COLOR_TEXT);

    /* Push just the line we touched, not the whole 720x720 frame -- see
     * the module comment for why bypassing window_present()/the
     * compositor queue here is safe (no window exists yet to race with).
     *
     * _draw()'s 3rd/4th args are END coordinates (esp_lcd_panel_draw_bitmap()
     * takes x_start/y_start/x_end/y_end, exclusive), not width/height --
     * confirmed against components/esp_lcd/include/esp_lcd_panel_ops.h.
     * compositor.c's only call site (0, 0, FRAMEBUFFER_MAX_W,
     * FRAMEBUFFER_MAX_H) can't disambiguate the two conventions since it
     * always draws the full frame from the origin; this line is not at the
     * origin, so getting this wrong would draw nothing or the wrong rows. */
    lcd->_draw(
        (void *)lcd,
        0,
        line_y,
        FRAMEBUFFER_MAX_W,
        line_y + FONT_HEIGHT * BOOT_PROGRESS_SCALE,
        fb + (size_t)line_y * FRAMEBUFFER_MAX_W
    );

    // Cosmetic pacing only -- see the module comment. Not load-bearing for
    // legibility the way the old fixed hold-delay was.
    vTaskDelay(pdMS_TO_TICKS(BOOT_LOG_MIN_STEP_MS));
}
