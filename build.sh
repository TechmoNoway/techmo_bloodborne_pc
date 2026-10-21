#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
export CC=clang CXX=clang++
export CFLAGS="${CFLAGS:-} -Wno-error=unused-but-set-global"
export CXXFLAGS="${CXXFLAGS:-} -include $(cygpath -m "$PWD/engine/native/gpu/win_compat.h")"
export BB_JOBS="${BB_JOBS:-2}" CMAKE_BUILD_PARALLEL_LEVEL="${BB_JOBS:-2}"
(cd engine/native && bash build.sh)
python -B tools/write_native_build_info.py
