#!/usr/bin/env bash
# Standalone build for the PAX prototype (task #83). Deliberately NOT wired
# into sdk_apps/CMakeLists.txt's build_app() -- that function only invokes
# CMAKE_C_COMPILER, and PAX's helpers/pax_dh_*.cpp are real C++ (see
# pax_test.c's top comment). This script mirrors build_app()'s exact
# compile/link flags (sdk_apps/CMakeLists.txt) but adds a g++ pass for the
# .cpp files.
#
# Usage (from a firmware checkout with sdk_dist/ already built via
# `idf.py sdk`, run inside the espressif/idf:v5.5.1 image so
# riscv32-esp-elf-{gcc,g++} are on PATH):
#
#   SDK_DIST=/path/to/firmware/sdk_dist bash build.sh
#
# Output: app_elfs/pax_test.elf

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAX_DIR="$SCRIPT_DIR/vendor/pax-graphics/core"
OUT_DIR="$SCRIPT_DIR/app_elfs"
OBJ_DIR="$SCRIPT_DIR/build_obj"
SDK_DIST="${SDK_DIST:?set SDK_DIST to a firmware sdk_dist/ dir (produced by idf.py sdk)}"

mkdir -p "$OUT_DIR" "$OBJ_DIR"

COMMON_FLAGS=(
    -O2 -fPIC -fdata-sections -ffunction-sections
    -fstrict-volatile-bitfields -fvisibility=hidden -g3
    -mabi=ilp32f -march=rv32imafc_zicsr_zifencei
)
# build_app()'s C-only flags (-fno-builtin -fno-builtin-function
# -fno-jump-tables -fno-tree-switch-conversion) are C-frontend options g++
# rejects for a mixed C/C++ TU set here, so PAX's C files pick them up too
# via CC_FLAGS below (they compile identically either way) and the .cpp
# files use CXX_FLAGS without them.
CC_FLAGS=("${COMMON_FLAGS[@]}" -fno-builtin -fno-builtin-function -fno-jump-tables -fno-tree-switch-conversion)
CXX_FLAGS=("${COMMON_FLAGS[@]}" -fno-exceptions -fno-rtti -fno-threadsafe-statics -std=gnu++17)
# SDK_DIST/include goes in via -idirafter, not -isystem/-I: it must be
# searched strictly AFTER the riscv32-esp-elf-g++ toolchain's own bundled
# libc/libstdc++ headers, not before. With -isystem it comes first and its
# picolibc sys/types.h shadows the toolchain's own pthread type defs --
# needed transitively by <memory> from pax_matrix.h's C++ helper block --
# breaking the g++ compile with "'pthread_t' does not name a type". Plain C
# PAX sources (e.g. pax_renderer_soft.c's endian.h) still need SDK_DIST as a
# fallback, since the toolchain's own bundled libc doesn't have every header.
PAX_INCLUDES=(-I "$PAX_DIR/include" -I "$PAX_DIR/src" -idirafter "$SDK_DIST/include")
APP_INCLUDES=("${PAX_INCLUDES[@]}" -I "$SCRIPT_DIR")
LINK_FLAGS=(-nostartfiles -nostdlib -shared -Wl,--strip-debug -Wl,--gc-sections -e main)

PAX_C_SRCS=(
    "$PAX_DIR/src/fonts/font_bitmap_7x9.c"
    "$PAX_DIR/src/fonts/font_bitmap_sky.c"
    "$PAX_DIR/src/fonts/font_bitmap_permanentmarker.c"
    "$PAX_DIR/src/fonts/font_bitmap_sairacondensed.c"
    "$PAX_DIR/src/fonts/font_bitmap_sairaregular.c"
    "$PAX_DIR/src/helpers/pax_precalculated.c"
    "$PAX_DIR/src/renderer/pax_renderer_soft.c"
    "$PAX_DIR/src/shapes/pax_arcs.c"
    "$PAX_DIR/src/shapes/pax_circles.c"
    "$PAX_DIR/src/shapes/pax_lines.c"
    "$PAX_DIR/src/shapes/pax_misc.c"
    "$PAX_DIR/src/shapes/pax_rects.c"
    "$PAX_DIR/src/shapes/pax_tris.c"
    "$PAX_DIR/src/pax_fonts.c"
    "$PAX_DIR/src/pax_gfx.c"
    "$PAX_DIR/src/pax_matrix.c"
    "$PAX_DIR/src/pax_orientation.c"
    "$PAX_DIR/src/pax_renderer.c"
    "$PAX_DIR/src/pax_setters.c"
    "$PAX_DIR/src/pax_shaders.c"
    "$PAX_DIR/src/pax_shapes.c"
    "$PAX_DIR/src/pax_text.c"
)

APP_C_SRCS=(
    "$SCRIPT_DIR/pax_test.c"
)

PAX_CXX_SRCS=(
    "$PAX_DIR/src/helpers/pax_dh_mcr_shaded.cpp"
    "$PAX_DIR/src/helpers/pax_dh_mcr_unshaded.cpp"
    "$PAX_DIR/src/helpers/pax_dh_shaded.cpp"
    "$PAX_DIR/src/helpers/pax_dh_unshaded.cpp"
)

VERSION_DEFS=(
    -D__PAX_VERSION_IS_SNAPSHOT=0 -D__PAX_VERSION_MAJOR=0 -D__PAX_VERSION_MINOR=0
    -D__PAX_VERSION_PATCH=0 -D__PAX_VERSION_NUMBER=000 -D__PAX_VERSION_STR="\"prototype\""
)

OBJS=()
for src in "${PAX_C_SRCS[@]}"; do
    obj="$OBJ_DIR/$(basename "$src").o"
    echo "CC  $(basename "$src")"
    riscv32-esp-elf-gcc "${CC_FLAGS[@]}" "${VERSION_DEFS[@]}" "${PAX_INCLUDES[@]}" -c "$src" -o "$obj"
    OBJS+=("$obj")
done
for src in "${PAX_CXX_SRCS[@]}"; do
    obj="$OBJ_DIR/$(basename "$src").o"
    echo "CXX $(basename "$src")"
    riscv32-esp-elf-g++ "${CXX_FLAGS[@]}" "${VERSION_DEFS[@]}" "${PAX_INCLUDES[@]}" -c "$src" -o "$obj"
    OBJS+=("$obj")
done
for src in "${APP_C_SRCS[@]}"; do
    obj="$OBJ_DIR/$(basename "$src").o"
    echo "CC  $(basename "$src") (app)"
    riscv32-esp-elf-gcc "${CC_FLAGS[@]}" "${VERSION_DEFS[@]}" "${APP_INCLUDES[@]}" -c "$src" -o "$obj"
    OBJS+=("$obj")
done

echo "LINK pax_test.elf"
riscv32-esp-elf-g++ "${COMMON_FLAGS[@]}" "${LINK_FLAGS[@]}" -o "$OUT_DIR/pax_test.elf" "${OBJS[@]}"

echo "OK: $OUT_DIR/pax_test.elf"
