#!/usr/bin/env bash
# Native Windows fixtures; no Bloodborne image or launcher configuration is loaded.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
CC=${CC:-clang}
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
libraries=("${libraries[@]/-mwindows/-mconsole}")
cstd=(-std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00)
runtime=(engine/runtime/runtime*.c engine/runtime/win32_*.c)
mapfile -t gpu < out/gpu/bbgpu_link.txt
link=(-Wl,--disable-dynamicbase,--disable-high-entropy-va -lwinmm -lws2_32 -lpsapi -flto=thin)
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror -I. tests/test_win32_diagnostics.c engine/runtime/win32_diagnostics.c -o out/diagnostics-test.exe
python -m unittest discover -s tests -p 'test_native_crash_reporter.py'
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${includes[@]}" -I. -Iengine/runtime tests/test_pad.c engine/runtime/runtime_host.c "${libraries[@]}" -o out/pad-test.exe
out/pad-test.exe
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror -I. -Iengine/runtime tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -lwinpthread -o out/runtime-test.exe
out/runtime-test.exe
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror -I. -Iengine/runtime tests/test_file_mods.c engine/runtime/win32_compat.c -lws2_32 -o out/file-mods-test.exe
out/file-mods-test.exe
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror -I. -Iengine/runtime tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -lwinpthread -o out/sema-test.exe
out/sema-test.exe
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror -I. -Iengine/runtime tests/test_content.c engine/runtime/runtime_content.c -o out/content-test.exe
out/content-test.exe
