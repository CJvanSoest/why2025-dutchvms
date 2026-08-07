// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the compositor's damage-rectangle maths. A wrong rectangle
// here does not crash, it leaves stale pixels on the panel, which is the
// expensive kind of bug to chase on hardware.

#include "rect_math.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                            \
        }                                                          \
    } while (0)

static long area(window_rect_t r) {
    return (long)r.w * (long)r.h;
}

// rect_subtract must produce pieces that exactly tile `a` minus the overlap,
// and must not overlap each other.
static void check_subtract(window_rect_t a, window_rect_t b, char const *what) {
    small_rect_array_t p = rect_subtract(a, b);

    long expected = area(a) - (rect_intersects(a, b) ? area(rect_intersection(a, b)) : 0);
    long got      = 0;
    for (int i = 0; i < p.count; i++) {
        got += area(p.rects[i]);
    }
    if (got != expected) {
        printf("FAIL: %s: pieces cover %ld, expected %ld\n", what, got, expected);
        failures++;
    }

    for (int i = 0; i < p.count; i++) {
        for (int j = i + 1; j < p.count; j++) {
            if (rect_intersects(p.rects[i], p.rects[j])) {
                printf("FAIL: %s: pieces %d and %d overlap\n", what, i, j);
                failures++;
            }
        }
        if (rect_intersects(p.rects[i], b)) {
            printf("FAIL: %s: piece %d still overlaps the subtracted rect\n", what, i);
            failures++;
        }
    }
}

int main(void) {
    window_rect_t const a = {100, 100, 200, 200};

    check_subtract(a, (window_rect_t){400, 400, 10, 10}, "no overlap");
    check_subtract(a, (window_rect_t){50, 50, 400, 400}, "fully covered");
    check_subtract(a, (window_rect_t){150, 150, 50, 50}, "hole in the middle");
    check_subtract(a, (window_rect_t){50, 150, 100, 50}, "bite from the left");
    check_subtract(a, (window_rect_t){250, 150, 200, 50}, "bite from the right");
    check_subtract(a, (window_rect_t){150, 50, 50, 100}, "bite from the top");
    check_subtract(a, (window_rect_t){150, 250, 50, 200}, "bite from the bottom");
    check_subtract(a, (window_rect_t){50, 50, 150, 400}, "left half covered");

    // merge_rectangles must coalesce an exact tiling back into one rectangle.
    rect_array_t arr = {0};
    arr.rects[0]     = (window_rect_t){0, 0, 10, 10};
    arr.rects[1]     = (window_rect_t){10, 0, 10, 10};
    arr.rects[2]     = (window_rect_t){0, 10, 20, 10};
    arr.count        = 3;
    merge_rectangles(&arr);
    CHECK(arr.count == 1, "three tiles merge into one");
    CHECK(arr.count == 1 && arr.rects[0].w == 20 && arr.rects[0].h == 20, "merged extent is 20x20");

    // Rectangles that do not touch must survive untouched.
    rect_array_t apart = {0};
    apart.rects[0]     = (window_rect_t){0, 0, 10, 10};
    apart.rects[1]     = (window_rect_t){50, 50, 10, 10};
    apart.count        = 2;
    merge_rectangles(&apart);
    CHECK(apart.count == 2, "disjoint rectangles are not merged");

    // The PPA workaround: N*32+1 splits, anything else does not.
    CHECK(is_problematic_block_height(65, 1.0f), "65 is a problematic height");
    CHECK(is_problematic_block_height(97, 1.0f), "97 is a problematic height");
    CHECK(!is_problematic_block_height(64, 1.0f), "64 is fine");
    CHECK(!is_problematic_block_height(33, 2.0f), "scale is applied before the test");

    rect_array_t split = {0};
    split.rects[0]     = (window_rect_t){0, 0, 100, 65};
    split.count        = 1;
    int dropped        = 0;
    CHECK(ppa_workaround_split_rects(&split, 1.0f, &dropped), "a problematic rect reports a split");
    CHECK(split.count == 2, "it becomes two rectangles");
    CHECK(dropped == 0, "nothing dropped with room to spare");
    CHECK(split.count == 2 && split.rects[0].h + split.rects[1].h == 65, "the halves still cover 65 rows");
    CHECK(split.count == 2 && split.rects[1].y == split.rects[0].y + split.rects[0].h, "the halves are adjacent");

    // A full array of problematic rectangles is where the first half used to be
    // written without a bounds check: rects[64] lands exactly on `count`, and
    // the following count++ then walks from whatever x happened to be.
    rect_array_t full = {0};
    for (int i = 0; i < MAX_VISIBLE_RECTS; i++) {
        full.rects[i] = (window_rect_t){0x41414141, i, 100, 65};
    }
    full.count = MAX_VISIBLE_RECTS;
    dropped    = 0;
    ppa_workaround_split_rects(&full, 1.0f, &dropped);
    CHECK(full.count >= 0 && full.count <= MAX_VISIBLE_RECTS, "count stays within the array");
    CHECK(dropped > 0, "dropping past the cap is reported, not silent");

    if (failures == 0) {
        printf("test_rect_math: all checks passed\n");
        return 0;
    }
    printf("test_rect_math: %d failure(s)\n", failures);
    return 1;
}
