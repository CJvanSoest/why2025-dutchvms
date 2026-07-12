# PAX prototype (task #83)

Standalone, build-verified-only prototype answering the open question in
[why2025-apps' `docs/pax_lvgl_design_proposal.md`](https://github.com/CJvanSoest/why2025-apps/blob/main/docs/pax_lvgl_design_proposal.md)
step 1: can [PAX](https://github.com/robotman2412/pax-graphics) draw a font
and basic primitives directly into a BadgeVMS window's own framebuffer?

**Status: builds and links to a valid PIE ELF. Not flashed or run on real
hardware.** No badge was available this session (see task #86).

## What this proves

- `pax_test.elf` links cleanly as a `DYN` RISC-V shared object with the
  expected undefined `window_*` symbols (`window_create`,
  `window_framebuffer_create`, `window_event_poll`, `window_present`) --
  the same shape as every other BadgeVMS app, resolved at load time by the
  kernel exactly like `framebuffer_test`.
- `pax_test.c` draws directly into the window's own framebuffer memory (no
  extra memcpy/blit): `pax_buf_init(&buf, framebuffer->pixels, w, h,
  PAX_BUF_16_565RGB)`.
- Rectangles (plain, outlined, rounded), a circle, a thick line, and text in
  the default bitmap font (`PAX_FONT_DEFAULT`) all compile against real PAX
  entry points (`pax_draw_rect`, `pax_outline_round_rect`, `pax_draw_circle`,
  `pax_draw_thick_line`, `pax_draw_text`).

## What this does NOT prove

- That `PAX_BUF_16_565RGB` is actually bit-identical to
  `BADGEVMS_PIXELFORMAT_RGB565` (both are 5-6-5 packed `uint16_t`, but byte
  order/channel order was never checked against real pixels).
- That it renders correctly, that timing/frame budget is acceptable, or
  that the malloc/free calls PAX makes internally (through the SDK's picolibc)
  behave correctly at runtime.
- Anything about the async/multicore renderer or PAX's `pax::` C++ helper
  API (`pax_matrix.h`) -- neither is used or exercised here (see below).

## The real finding: PAX needs a C++ compiler, and that's not free

`docs/pax_lvgl_design_proposal.md` characterized PAX as "add a new
dependency" without flagging that it needs C++. That undersold the cost --
this should be corrected. Specifics:

- `core/CMakeLists.txt`'s source list unconditionally includes 4 `.cpp`
  files (`helpers/pax_dh_{mcr_,}{,un}shaded.cpp`) -- PAX's actual pixel
  drawing helpers, not optional.
- `sdk_apps/CMakeLists.txt`'s `build_app()` only ever invokes
  `${CMAKE_C_COMPILER}` (`riscv32-esp-elf-gcc`). There is no C++ support in
  the shared app build pipeline today.
- `riscv32-esp-elf-g++` **does** exist in the `espressif/idf:v5.5.1` image
  used for every NAS build in this fork, so this isn't a missing-toolchain
  blocker -- but wiring real C++ support into `build_app()` (a second
  compiler, mixed-language `SOURCES`, etc.) is a genuine, permanent change
  to shared build infrastructure, which is why this prototype avoids it
  entirely (see `build.sh` below) rather than deciding that trade-off here.
- The 4 `.cpp` files themselves turned out to be plain C wearing a `.cpp`
  extension (macro-based generics via `#include "*.inc"`, no `std::`, no
  classes, no exceptions) -- **except** that `pax_matrix.h` has a genuine,
  always-compiled C++ block (`#ifdef __cplusplus`) with real templates and
  an `operator""_fix` user-defined literal, which pulls in `<memory>` ->
  libstdc++'s `<bits/gthr.h>` -> `pthread_t` and friends. BadgeVMS's SDK
  picolibc (`sdk_dist/include`) doesn't define real pthread types, and
  feeding that include path to the C++ compile (as `-isystem`, ahead of the
  toolchain's own bundled libstdc++/libc headers) breaks that chain with
  `'pthread_t' does not name a type`. Fixed here by passing `sdk_dist/include`
  via `-idirafter` instead, so the toolchain's own bundled headers win first
  and the SDK is only a fallback (needed for e.g. `endian.h`, which the
  toolchain's own bundled libc lacks) -- see the comment in `build.sh`.
- `core/src/ptq/` (a `pthreadqueue` git submodule) and
  `renderer/pax_renderer_softasync.c` (PAX's async/multicore renderer) are
  excluded from this prototype entirely -- out of scope for "font +
  drawing primitives", and would need actual pthread support from the SDK
  to build, which doesn't exist here.

## Why this isn't wired into `sdk_apps/CMakeLists.txt`

Per the design proposal's own step 1 ("losstaand prototype, niet direct in
de launcher"): this is a standalone experiment, not a real build-pipeline
change. `build.sh` in this directory compiles PAX's C sources with
`riscv32-esp-elf-gcc`, the 4 `.cpp` files with `riscv32-esp-elf-g++`
(mirroring `build_app()`'s exact compile/link flags), and links everything
into `pax_test.elf` in one extra pass -- without touching
`sdk_apps/CMakeLists.txt`. If PAX is ever adopted for real (cj_launcher or
elsewhere), `build_app()` would need real multi-language `SOURCES` support;
that's a separate, deliberate decision, not something this prototype forces.

## Building

From a firmware checkout with `sdk_dist/` already produced (`idf.py sdk`),
inside the `espressif/idf:v5.5.1` image so `riscv32-esp-elf-{gcc,g++}` are on
`PATH`:

```sh
. $IDF_PATH/export.sh
SDK_DIST=/path/to/firmware/sdk_dist bash sdk_apps/pax_test/build.sh
```

Output: `sdk_apps/pax_test/app_elfs/pax_test.elf`.

## `vendor/pax-graphics/`

Real upstream PAX source (`git clone --depth 1
https://github.com/robotman2412/pax-graphics.git`, MIT-licensed, `LICENSE`
carried alongside), trimmed to only what this prototype needs -- see the
"excluded" note above for what was deliberately left out.
