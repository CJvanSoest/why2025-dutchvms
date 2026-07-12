#!/usr/bin/env bash
# Standalone build for the LVGL prototype (task #84). Deliberately NOT wired
# into sdk_apps/CMakeLists.txt's build_app() (see the task #83 PAX prototype
# for why that matters there); LVGL core is plain C99 with LV_USE_OS left at
# LV_OS_NONE here, so unlike PAX this has no C++ toolchain question -- kept
# standalone only to match the "losstaand prototype" framing from
# docs/pax_lvgl_design_proposal.md, not because of a real build_app() gap.
#
# Usage (from a firmware checkout with sdk_dist/ already built via
# `idf.py sdk`, run inside the espressif/idf:v5.5.1 image so
# riscv32-esp-elf-gcc is on PATH):
#
#   SDK_DIST=/path/to/firmware/sdk_dist bash build.sh
#
# Output: app_elfs/lvgl_test.elf

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LVGL_DIR="$SCRIPT_DIR/vendor/lvgl"
OUT_DIR="$SCRIPT_DIR/app_elfs"
OBJ_DIR="$SCRIPT_DIR/build_obj"
SDK_DIST="${SDK_DIST:?set SDK_DIST to a firmware sdk_dist/ dir (produced by idf.py sdk)}"

mkdir -p "$OUT_DIR" "$OBJ_DIR"

CC_FLAGS=(
    -O2 -fPIC -fdata-sections -ffunction-sections
    -fno-builtin -fno-builtin-function -fno-jump-tables -fno-tree-switch-conversion
    -fstrict-volatile-bitfields -fvisibility=hidden -g3
    -mabi=ilp32f -march=rv32imafc_zicsr_zifencei
)
LINK_FLAGS=(-nostartfiles -nostdlib -shared -Wl,--strip-debug -Wl,--gc-sections -e main)
# SDK headers/lv_conf.h come after LVGL's own -- LVGL doesn't need anything
# from the SDK, but lv_conf.h's LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB path
# calls plain malloc/free/etc., which the SDK provides.
INCLUDES=(
    -I "$SCRIPT_DIR"                # lv_conf.h (LV_CONF_INCLUDE_SIMPLE picks this up via -I)
    -I "$LVGL_DIR"
    -isystem "$SDK_DIST/include"
)
DEFINES=(-DLV_CONF_INCLUDE_SIMPLE)

mapfile -t LVGL_SRCS < <(find "$LVGL_DIR/src" -name '*.c' | sort)
echo "Found ${#LVGL_SRCS[@]} LVGL source files"

OBJS=()
for src in "${LVGL_SRCS[@]}"; do
    rel="${src#"$LVGL_DIR"/}"
    obj="$OBJ_DIR/${rel//\//_}.o"
    riscv32-esp-elf-gcc "${CC_FLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
    OBJS+=("$obj")
done
echo "CC  lvgl core (${#LVGL_SRCS[@]} files)"

echo "CC  lvgl_test.c"
riscv32-esp-elf-gcc "${CC_FLAGS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" -c "$SCRIPT_DIR/lvgl_test.c" -o "$OBJ_DIR/lvgl_test.c.o"
OBJS+=("$OBJ_DIR/lvgl_test.c.o")

echo "LINK lvgl_test.elf"
riscv32-esp-elf-gcc "${CC_FLAGS[@]}" "${LINK_FLAGS[@]}" -o "$OUT_DIR/lvgl_test.elf" "${OBJS[@]}"

echo "OK: $OUT_DIR/lvgl_test.elf"
