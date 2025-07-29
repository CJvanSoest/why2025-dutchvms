#!/bin/sh
PATHS="$(find main apps -maxdepth 1 \! -name thirdparty -type d) include/badgevms"
find ${PATHS} -maxdepth 1 -name '*.c' -exec clang-format -i {} \;
find ${PATHS} -maxdepth 1 -name '*.h' -exec clang-format -i {} \;
