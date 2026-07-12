// SPDX-License-Identifier: MIT
//
// Standalone prototype for task #84 (docs/design/pax_lvgl_design_proposal.md,
// why2025-apps repo, Track 2): a display driver (flush_cb into a BadgeVMS
// window's framebuffer) + an input driver (BadgeVMS keyboard events -> LVGL
// keypad indev) for LVGL 9.
//
// Not wired into sdk_apps/CMakeLists.txt / build_app() -- see build.sh in
// this directory. Unlike the PAX prototype (task #83), LVGL core is plain
// C99 with LV_USE_OS left at LV_OS_NONE, so there's no C++ toolchain
// question here; this is a plain single-compiler build, kept standalone
// only to match the "losstaand prototype" framing, not because of a real
// build_app() limitation.

#include "badgevms/compositor.h"
#include "badgevms/event.h"
#include "badgevms/framebuffer.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FB_WIDTH  (360)
#define FB_HEIGHT (360)

static framebuffer_t *g_framebuffer;
static window_handle_t g_window;

static uint32_t tick_get_cb(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

// PAX_BUF_16_565RGB-style assumption carried over from the task #83
// prototype: LV_COLOR_DEPTH 16 is assumed bit-identical to
// BADGEVMS_PIXELFORMAT_RGB565. Unverified on real hardware.
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint16_t *src = (uint16_t *)px_map;
    uint16_t *dst = (uint16_t *)g_framebuffer->pixels;
    int32_t   row_len = area->x2 - area->x1 + 1;
    for (int32_t y = area->y1; y <= area->y2; y++) {
        memcpy(&dst[y * FB_WIDTH + area->x1], src, row_len * sizeof(uint16_t));
        src += row_len;
    }
    if (lv_display_flush_is_last(disp)) {
        window_present(g_window, true, NULL, 0);
    }
    lv_display_flush_ready(disp);
}

// Minimal keypad indev: BadgeVMS gives us discrete key-down/up events, LVGL
// wants a poll-style read_cb reporting the *current* key + state. Latch the
// most recent event between poll calls -- good enough for a prototype, a
// real integration would want a small queue so fast key sequences can't
// skip a state.
static uint32_t g_last_lv_key;
static bool     g_last_key_pressed;

static uint32_t map_scancode(keyboard_scancode_t sc) {
    switch (sc) {
        case KEY_SCANCODE_RETURN:
        case KEY_SCANCODE_SPACE:
            return LV_KEY_ENTER;
        case KEY_SCANCODE_TAB:
        case KEY_SCANCODE_DOWN:
        case KEY_SCANCODE_RIGHT:
            return LV_KEY_NEXT;
        case KEY_SCANCODE_UP:
        case KEY_SCANCODE_LEFT:
            return LV_KEY_PREV;
        default:
            return 0;
    }
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    data->key   = g_last_lv_key;
    data->state = g_last_key_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int main(int argc, char *argv[]) {
    window_size_t size = {FB_WIDTH, FB_HEIGHT};
    g_window            = window_create("LVGL test", size, WINDOW_FLAG_DOUBLE_BUFFERED);
    g_framebuffer        = window_framebuffer_create(g_window, size, BADGEVMS_PIXELFORMAT_RGB565);

    lv_init();
    lv_tick_set_cb(tick_get_cb);

    lv_display_t *disp = lv_display_create(FB_WIDTH, FB_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static uint8_t draw_buf[FB_WIDTH * 40 * sizeof(uint16_t)]; // 40 rows, partial-render
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, indev_read_cb);

    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "LVGL prototype");
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Press Enter");

    lv_group_t *group = lv_group_create();
    lv_group_add_obj(group, btn);
    lv_indev_set_group(indev, group);
    lv_group_focus_obj(btn);

    bool running = true;
    while (running) {
        event_t e = window_event_poll(g_window, false, 0);
        if (e.type == EVENT_KEY_DOWN || e.type == EVENT_KEY_UP) {
            if (e.keyboard.scancode == KEY_SCANCODE_ESCAPE && e.keyboard.down) {
                running = false;
            }
            g_last_lv_key       = map_scancode(e.keyboard.scancode);
            g_last_key_pressed = e.keyboard.down;
        }

        lv_timer_handler();
        usleep(5000);
    }

    printf("lvgl_test exiting\n");
    return 0;
}
