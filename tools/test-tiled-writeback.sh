#!/usr/bin/env bash
# Real GPU readback and collection over fixture memory, never game data.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" gc-policy-test memory-notifications-test tiled-writeback-test tiled-gc-test stable-arena-test image-hash-test
root=${BB_TILED_TEST_DIR:-out/test-fixtures/tiled-gc}
mkdir -p "$root"
out/gpu/gc-policy-test.exe
out/gpu/memory-notifications-test.exe
BB_COPY_THREADS=0 out/gpu/image-hash-test.exe
BB_COPY_THREADS=2 out/gpu/image-hash-test.exe
for mode in async sync; do
    export BB_VK_RECORD_THREAD=1 BB_VK_SEGMENT_KB=1
    if [[ $mode == async ]]; then export BB_ASYNC_SUBMIT=1 BB_VK_RECORD_THREADS=3
    else export BB_ASYNC_SUBMIT=0 BB_VK_RECORD_THREADS=1; fi
    out/gpu/stable-arena-test.exe > "$root/stable-arena-$mode.log" 2>&1
    out/gpu/stable-arena-test.exe --small-pages > "$root/stable-arena-small-$mode.log" 2>&1
    out/gpu/tiled-writeback-test.exe > "$root/roundtrip-$mode.log" 2>&1
    out/gpu/tiled-gc-test.exe safety > "$root/safety-$mode.log" 2>&1
    out/gpu/tiled-gc-test.exe lifetime > "$root/lifetime-$mode.log" 2>&1
    BB_IMAGE_RECYCLE_MB=16 out/gpu/tiled-gc-test.exe recycle > "$root/recycle-$mode.log" 2>&1
    BB_IMAGE_RECYCLE_MB=0 out/gpu/tiled-gc-test.exe recycle > "$root/recycle-off-$mode.log" 2>&1
    out/gpu/tiled-gc-test.exe gpu-sync > "$root/gpu-sync-$mode.log" 2>&1
    BB_REUSE_GPU_SOURCE_HASH=0 out/gpu/tiled-gc-test.exe gpu-sync > "$root/gpu-sync-full-hash-$mode.log" 2>&1
    out/gpu/tiled-gc-test.exe refresh > "$root/refresh-$mode.log" 2>&1
    BB_KEEP_MIP_HASHES=0 out/gpu/tiled-gc-test.exe refresh > "$root/refresh-legacy-$mode.log" 2>&1
    for state in default off on; do
        out/gpu/tiled-gc-test.exe "$state" "$root/$state-$mode.csv" > "$root/$state-$mode.log" 2>&1
    done
    echo "PASS: tiled overlay, guarded cache writeback and default/off/on collection ($mode)"
done
