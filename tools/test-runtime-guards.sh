#!/usr/bin/env bash
# Device-free production failure-path checks. All saves and dumps are synthetic.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
CC=${CC:-clang}
CXX=${CXX:-clang++}
cflags=(-std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00 -O2 -g -flto=thin -Wall -Wextra -Werror -I. -Iengine/runtime)
cxxflags=(-std=c++23 -DFMT_HEADER_ONLY -DNDEBUG -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -O2 -g -flto=thin -Wall -Wextra -Werror -Iengine/renderer/shim -isystem engine/renderer/shadps4 -isystem engine/renderer/third_party/gcn/include -isystem engine/renderer/third_party/half/include)
"$CC" "${cflags[@]}" tests/test_low_alloc.c engine/runtime/win32_low_alloc.c -o out/low-alloc-test.exe
out/low-alloc-test.exe
"$CC" "${cflags[@]}" tests/test_memory_probe.c engine/runtime/win32_memory.c -o out/memory-probe-test.exe
out/memory-probe-test.exe
python -m unittest discover -s tests -p 'test_memory_lease_native.py'
"$CC" "${cflags[@]}" -ffunction-sections -fdata-sections tests/test_audio_allocations.c out/libatrac9.a -Wl,--gc-sections -o out/audio-allocations-test.exe
out/audio-allocations-test.exe
"$CC" "${cflags[@]}" -ffunction-sections -fdata-sections tests/test_save_allocations.c engine/runtime/win32_compat.c -Wl,--gc-sections -lws2_32 -o out/save-allocations-test.exe
temp_root=${BB_TEST_TMPDIR:-out/test-fixtures}
mkdir -p "$temp_root"
save_fixture=$(mktemp -d "$temp_root/save-allocation-XXXXXX")
out/save-allocations-test.exe "$save_fixture"
"$CC" "${cflags[@]}" -c engine/runtime/win32_diagnostics.c -o out/diagnostics-test.o
"$CC" "${cflags[@]}" tests/test_win32_diagnostics.c out/diagnostics-test.o -o out/diagnostics-test.exe
"$CXX" "${cxxflags[@]}" tests/test_renderer_assert.cpp tests/test_log_sink.cpp engine/renderer/shim/common/assert.cpp out/diagnostics-test.o -lwinpthread -o out/renderer-assert-test.exe
"$CXX" "${cxxflags[@]}" tests/test_user_data.cpp tests/test_log_sink.cpp engine/renderer/shim/common/assert.cpp engine/renderer/shadps4/shader_recompiler/compile_memory.cpp -lwinpthread -o out/user-data-test.exe
out/user-data-test.exe
python -m unittest discover -s tests -p 'test_native_crash_reporter.py'
