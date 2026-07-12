// SPDX-License-Identifier: MIT
//
// Standalone prototype for task #83 (docs/design/pax_lvgl_design_proposal.md,
// why2025-apps repo): can PAX (github.com/robotman2412/pax-graphics) draw a
// font + basic primitives directly into a BadgeVMS window's own framebuffer?
//
// Not wired into sdk_apps/CMakeLists.txt / build_app() -- see build.sh in
// this directory. build_app() only invokes CMAKE_C_COMPILER; PAX's 4
// drawing-helper translation units are genuinely C++ (pax_fixpt.hpp uses a
// user-defined literal + constexpr), so this needs its own two-compiler
// build until/unless build_app() grows real C++ support.

#include "badgevms/compositor.h"
#include "badgevms/event.h"
#include "badgevms/framebuffer.h"

#include "pax_gfx.h"

#include <stdio.h>
#include <unistd.h>

#define FB_WIDTH  (360)
#define FB_HEIGHT (360)

int main(int argc, char *argv[]) {
    window_size_t   size       = {FB_WIDTH, FB_HEIGHT};
    window_handle_t window     = window_create("PAX test", size, WINDOW_FLAG_DOUBLE_BUFFERED);
    framebuffer_t   *framebuffer = window_framebuffer_create(window, size, BADGEVMS_PIXELFORMAT_RGB565);

    // Draw straight into the window's own framebuffer -- PAX_BUF_16_565RGB
    // is assumed bit-identical to BADGEVMS_PIXELFORMAT_RGB565 (both 5-6-5
    // packed uint16_t); unverified on real hardware, see README.md.
    pax_buf_t buf;
    if (!pax_buf_init(&buf, framebuffer->pixels, FB_WIDTH, FB_HEIGHT, PAX_BUF_16_565RGB)) {
        printf("pax_buf_init failed\n");
        return 1;
    }

    int frame = 0;
    while (1) {
        event_t e = window_event_poll(window, false, 0);
        if (e.type == EVENT_KEY_DOWN && e.keyboard.scancode == KEY_SCANCODE_ESCAPE) {
            break;
        }

        pax_draw_rect(&buf, 0xff202030, 0, 0, FB_WIDTH, FB_HEIGHT);
        pax_draw_rect(&buf, 0xffe0a030, 20, 20, 140, 50);
        pax_outline_round_rect(&buf, 0xffffffff, 20, 20, 140, 50, 8);
        pax_draw_circle(&buf, 0xff40c0ff, 280, 60, 30);
        pax_draw_thick_line(&buf, 0xff80ff80, 20, 100, 340, 100, 3);

        pax_draw_text(&buf, 0xffffffff, PAX_FONT_DEFAULT, 18, 20, 120, "PAX prototype");

        char frame_label[32];
        snprintf(frame_label, sizeof(frame_label), "frame %d", frame);
        pax_draw_text(&buf, 0xffa0a0ff, PAX_FONT_DEFAULT, 14, 20, 150, frame_label);

        pax_join();

        window_present(window, true, NULL, 0);
        frame++;
        usleep(16667);
    }
    printf("pax_test exiting\n");
    return 0;
}
