#!/usr/bin/env bash
# Shader translation and binary-cache regressions; no Vulkan device or game.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" shader-correctness-test shader-memory-test motion-shader-test render-semantics-test image-layout-cache-test lru-cache-test gc-policy-test memory-notifications-test
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" shader-cache-input-test
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" image-allocation-cache-test
out/gpu/image-allocation-cache-test.exe
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" index-buffer-range-test prepared-vertex-inputs-test
out/gpu/index-buffer-range-test.exe
out/gpu/prepared-vertex-inputs-test.exe
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" copy-fault-relay-test
out/gpu/copy-fault-relay-test.exe
out/gpu/copy-fault-relay-test.exe --serial
out/gpu/copy-fault-relay-test.exe --disabled
root=${BB_SHADER_TEST_DIR:-out/test-fixtures/shader-correctness}
mkdir -p "$root"
out/gpu/shader-cache-input-test.exe "$root/cache-rebuild"
for shader in "$root/cache-rebuild/clip-specialization"/*.spv; do spirv-val --target-env vulkan1.3 "$shader"; done
out/gpu/shader-correctness-test.exe all "$root/undef-u64.spv"
out/gpu/shader-memory-test.exe "$root"
out/gpu/motion-shader-test.exe "$root"
out/gpu/render-semantics-test.exe "$root"
out/gpu/image-layout-cache-test.exe
out/gpu/lru-cache-test.exe
out/gpu/gc-policy-test.exe
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" hdr-output-test
out/gpu/hdr-output-test.exe
out/gpu/memory-notifications-test.exe
for shader in "$root"/*.spv; do spirv-val --target-env vulkan1.3 "$shader"; done
echo 'PASS: SPIR-V validation and current/obsolete plain/motion shader cache versions'
