#!/bin/sh
PATHS="$(find badgevms badgevms/include sdk_apps badgevms/drivers/esp-serial-flasher -maxdepth 1 \! -name thirdparty -type d)"
find ${PATHS} -maxdepth 1 -name '*.c' -exec clang-format -i {} \;
find ${PATHS} -maxdepth 1 -name '*.h' -exec clang-format -i {} \;
