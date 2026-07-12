# LVGL prototype (task #84)

Standalone, build-verified-only prototype answering the open question in
[why2025-apps' `docs/pax_lvgl_design_proposal.md`](https://github.com/CJvanSoest/why2025-apps/blob/main/docs/pax_lvgl_design_proposal.md)
Track 2 step 2: can [LVGL](https://github.com/lvgl/lvgl) (v9.3) get a
display driver + input driver wired up on top of BadgeVMS?

**Status: builds and links to a valid PIE ELF. Not flashed or run on real
hardware.** No badge was available this session (see task #86).

## What this proves

- `lvgl_test.elf` links cleanly as a `DYN` RISC-V shared object with the
  expected undefined `window_*` symbols plus `malloc`/`free` (LVGL's
  `LV_USE_STDLIB_MALLOC = LV_STDLIB_CLIB`, resolved against the SDK's
  picolibc at load time, same as any other app) -- the same shape as every
  other BadgeVMS app.
- A display driver: `lv_display_create()` + `lv_display_set_flush_cb()`
  copying LVGL's partial render buffer into the window's own
  `framebuffer_t->pixels`, then `window_present()` on the last flush of a
  frame (`lv_display_flush_is_last()`).
- An input driver: `lv_indev_create()` with `LV_INDEV_TYPE_KEYPAD`, fed from
  `window_event_poll()` -- BadgeVMS key-down/up events mapped to
  `LV_KEY_ENTER`/`LV_KEY_NEXT`/`LV_KEY_PREV`, driving a focus group
  (`lv_group_t`) over a button widget.
- A tick source: `lv_tick_set_cb()` backed by `clock_gettime(CLOCK_MONOTONIC)`,
  same pattern `framebuffer_test_a.c` already uses for FPS timing.
- A label + a button widget render against LVGL's default bitmap font
  (Montserrat 14) without any custom font tooling.

## What this does NOT prove

- That `LV_COLOR_FORMAT_RGB565` is bit-identical to
  `BADGEVMS_PIXELFORMAT_RGB565` (same unverified assumption as the PAX
  prototype -- both are 5-6-5 packed `uint16_t`, channel/byte order never
  checked against real pixels).
- That it renders correctly, that the partial-render 40-row buffer strikes
  a good latency/RAM trade-off, or that `malloc`/`free` through the SDK's
  picolibc behave well under LVGL's allocation pattern at runtime.
- PSRAM headroom -- the design proposal flagged this as unmeasured on this
  specific P4 board; this prototype doesn't touch it either.
- Anything about LVGL's OS integration (`LV_USE_OS`), animations, styles
  beyond defaults, or any widget besides label/button.

## Unlike the PAX prototype: no C++ question here

Task #83's PAX prototype needed a second compiler pass
(`riscv32-esp-elf-g++`) because PAX's drawing helpers are genuinely C++.
LVGL 9's core is plain C99, and this prototype leaves `LV_USE_OS` at its
default `LV_OS_NONE` (BadgeVMS apps are already single-threaded processes;
no need for LVGL's own pthread/FreeRTOS OSAL), so there's no equivalent
`<memory>`/pthread-type conflict to work around. `build.sh` is a single
`riscv32-esp-elf-gcc` pass over the entire vendored `src/` tree (LVGL's
supported way to build it: every file is always compiled, and `lv_conf.h`'s
`LV_USE_*` macros gate what's actually included).

## Two known config gotchas applied ([[lvgl_image_decode_gotchas]])

Already hit and fixed once before on a different ESP32-S3 badge (LilyGo
T-Pager) in this fork's history -- applied here from the start rather than
rediscovered:

- `LV_USE_STDLIB_MALLOC` set to `LV_STDLIB_CLIB` (not the default 64 KB
  built-in pool) -- the built-in pool fails outright on anything beyond
  small allocations; CLIB routes through the SDK's real `malloc`/`free`.
- `LV_CACHE_DEF_SIZE` set to a non-zero `4 * 1024U` (not the default `0`) --
  relevant if image decode caching is ever exercised; this prototype
  doesn't decode images, but the gotcha is set defensively since it's free.

## Why this isn't wired into `sdk_apps/CMakeLists.txt`

Same "losstaand prototype" framing as task #83 -- a standalone experiment,
not a real build-pipeline change. `build.sh` in this directory compiles the
entire vendored LVGL `src/` tree plus `lvgl_test.c` with
`riscv32-esp-elf-gcc` directly, without touching `sdk_apps/CMakeLists.txt`.
If LVGL is ever adopted for a real app (`cj_meshcore` is the design
proposal's suggested first candidate), it would make more sense to build it
as a proper `sdk_libs/` library (that tree already supports mixed compilers
via CMake's native per-file toolchain selection, unlike `build_app()`) --
that's a separate, deliberate decision, not something this prototype forces.

## Building

From a firmware checkout with `sdk_dist/` already produced (`idf.py sdk`),
inside the `espressif/idf:v5.5.1` image so `riscv32-esp-elf-gcc` is on
`PATH`:

```sh
. $IDF_PATH/export.sh
SDK_DIST=/path/to/firmware/sdk_dist bash sdk_apps/lvgl_test/build.sh
```

Output: `sdk_apps/lvgl_test/app_elfs/lvgl_test.elf`.

## `vendor/lvgl/`

Real upstream LVGL `release/v9.3` source (`git clone --depth 1 --branch
release/v9.3 https://github.com/lvgl/lvgl.git`, MIT-licensed, `LICENCE.txt`
carried alongside). Trimmed from the full clone (235 MB with docs/examples/
demos/tests) down to `src/` + the top-level headers (`lvgl.h`,
`lvgl_private.h`, `lv_version.h`), then further trimmed within `src/font/`
to only the default Montserrat-14 font plus the loader machinery it needs
(`lv_font.c`, `lv_font_fmt_txt.c`, `lv_binfont_loader.c`) -- the other ~30
bundled font sizes/scripts were removed, they're not referenced by
`lv_conf.h`. `src/libs/` (third-party codec bindings: PNG, JPEG, GIF, QR,
barcode, etc.) is kept even though none are enabled in `lv_conf.h`, because
`lvgl.h` `#include`s those headers unconditionally regardless of the
`LV_USE_*` gate on their contents -- removing the directory breaks the
build with a missing-header error, discovered the first time this was
tried here.
