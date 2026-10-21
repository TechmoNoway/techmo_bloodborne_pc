#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
# Windows engine build, run in MSYS2 CLANG64.
case $(uname -s) in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) echo 'This source targets Windows only. Run build.sh in MSYS2 CLANG64.' >&2; exit 1 ;;
esac
if [[ ${MSYSTEM:-} != CLANG64 ]]; then
    echo 'Open the MSYS2 CLANG64 shell before building (MSYSTEM=CLANG64).' >&2
    exit 1
fi
export CC=${CC:-clang}
export CXX=${CXX:-clang++}
if ! { command -v "$CC" >/dev/null && command -v "$CXX" >/dev/null &&
       command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 &&
       command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    echo 'Need MSYS2 CLANG64 compilers, Vulkan/SDL3 packages, cmake and ninja. See README.md.' >&2
    exit 1
fi
mkdir -p out
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
libraries=("${libraries[@]/-mwindows/-mconsole}")
cstd=(-std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00)
# GPU static library (shadPS4 video core + drivers): out/gpu/libbbgpu.a.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Pinned third-party sources are vendored; GitHub source ZIPs are complete.
for dependency in engine/renderer/third_party/fsr-vulkan/CMakeLists.txt \
                  engine/renderer/third_party/imgui/imgui.h third_party/LibAtrac9/C/src/decoder.c; do
    if [[ ! -f $dependency ]]; then
        echo "Missing vendored dependency: $dependency. Restore the complete source checkout." >&2
        exit 1
    fi
done
for patch in engine/renderer/patches/fsr-vulkan/*.patch; do
    if ! git apply --directory=engine/renderer/third_party/fsr-vulkan --reverse --check "$PWD/$patch" 2>/dev/null; then
        git apply --directory=engine/renderer/third_party/fsr-vulkan "$PWD/$patch"
    fi
done
cmake -S engine/renderer -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" >/dev/null
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here to prevent silently using an older static library.
if ! ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" bbgpu bb-engine-assets > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
cp -f out/gpu/bb-engine-assets.exe out/
# The reporter is separate so DbgHelp does not run inside a faulting game process.
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror -I. tools/crash_reporter.c -ldbghelp -o out/bb-crash-reporter.exe
# The GPU static library and its transitive dependencies, in link order.
mapfile -t gpu < out/gpu/bbgpu_link.txt
runtime=(engine/runtime/runtime*.c engine/runtime/win32_*.c)
link=(-Wl,--disable-dynamicbase,--disable-high-entropy-va -lwinmm -lws2_32 -lpsapi -flto=thin)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
# Windows: the link includes the GPU library's link-time optimization (a minute or more), and
# run.bat builds before every start, so an up-to-date executable is kept.
gpu_inputs=(out/gpu/libbbgpu.a out/gpu/bbgpu_link.txt)
# Native bridge dependencies can change without changing libbbgpu.a itself.
for dependency in "${gpu[@]}"; do
    [[ ! -f $dependency ]] || gpu_inputs+=("$dependency")
done
if [[ -f out/bb-probe.exe && -f out/bb-shader-lab.exe && -f out/bb-gpu-capabilities.exe &&
      -z $(find engine/runtime engine/address_space.h engine/renderer/bbgpu.h "${gpu_inputs[@]}" out/libatrac9.a build.sh \
               tools/gpu_capabilities.c tools/shader_lab.c -newer out/bb-probe.exe -print -quit) ]]; then
    echo "Up to date: $PWD/out/bb-probe.exe"
else
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${includes[@]}" -I. -Iengine/runtime engine/runtime/probe.c "${runtime[@]}" engine/runtime/vulkan_smoke.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o out/bb-probe.exe
echo "Built $PWD/out/bb-probe.exe"
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${includes[@]}" -I. -Iengine/runtime tools/shader_lab.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o out/bb-shader-lab.exe
# GPU and controller inventory; keep the Windows C dialect and include SDL headers.
"$CC" "${cstd[@]}" -O2 -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities.exe
fi
if [[ ${1:-} == --test ]]; then
    bash tools/test-native.sh
fi
python tools/write_native_build_info.py
