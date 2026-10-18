#!/usr/bin/env bash
# Shader translation and binary-cache regressions; no Vulkan device or game.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" shader-correctness-test shader-memory-test motion-shader-test render-semantics-test image-layout-cache-test lru-cache-test gc-policy-test memory-notifications-test
root=${BB_SHADER_TEST_DIR:-out/test-fixtures/shader-correctness}
mkdir -p "$root"
out/gpu/shader-correctness-test.exe all "$root/undef-u64.spv"
out/gpu/shader-memory-test.exe "$root"
out/gpu/motion-shader-test.exe "$root"
out/gpu/render-semantics-test.exe "$root"
out/gpu/image-layout-cache-test.exe
out/gpu/lru-cache-test.exe
out/gpu/gc-policy-test.exe
out/gpu/memory-notifications-test.exe
for shader in "$root"/*.spv; do spirv-val --target-env vulkan1.3 "$shader"; done
echo 'PASS: SPIR-V validation and current/obsolete plain/motion shader cache versions'
