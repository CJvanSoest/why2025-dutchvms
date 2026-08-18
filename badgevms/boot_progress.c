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

#include <string.h>

static char const *TAG = "boot_progress";

/* A thin strip along the bottom edge -- FONT_HEIGHT (7px) plus padding.
 * Deliberately tiny: pixel_functions.h's font is a fixed 5x7 bitmap font
 * with no scaling primitive at this level (that's a PAX/app-side thing,
 * not available yet this early in boot). */
#define BOOT_PROGRESS_BAR_H 20
#define BOOT_PROGRESS_TEXT_X 10
#define BOOT_PROGRESS_COLOR_BG   0x0000 /* black */
#define BOOT_PROGRESS_COLOR_TEXT 0xFFFF /* white */

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

    int bar_y = FRAMEBUFFER_MAX_H - BOOT_PROGRESS_BAR_H;
    int text_y = bar_y + (BOOT_PROGRESS_BAR_H - FONT_HEIGHT) / 2;

    /* Clear the whole strip first -- msg lengths vary between calls, and
     * we're not tracking the previous line's width to do a narrower clear. */
    draw_filled_rect_rotated(fb, 0, bar_y, FRAMEBUFFER_MAX_W, BOOT_PROGRESS_BAR_H, BOOT_PROGRESS_COLOR_BG);
    draw_text_rotated(fb, msg, BOOT_PROGRESS_TEXT_X, text_y, BOOT_PROGRESS_COLOR_TEXT);

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
}
