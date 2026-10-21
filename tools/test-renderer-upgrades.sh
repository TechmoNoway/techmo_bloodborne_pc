#!/usr/bin/env bash
# Synthetic Vulkan fixtures; never loads the game or player saves.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
root=out/test-fixtures/renderer-upgrades
mkdir -p "$root/user"
export BB_GPU_USER_DIR="$PWD/$root/user"
ninja -C out/gpu -j "${BB_BUILD_JOBS:-2}" native-constants-test fsr-preparation-test memory-pressure-test
out/gpu/memory-pressure-test.exe
out/gpu/native-constants-test.exe "$root"
for shader in "$root"/*.spv; do spirv-val --target-env vulkan1.3 "$shader"; done
out/gpu/fsr-preparation-test.exe
if [[ -f fsr4_shaders/fsr4_model_v07_i8_quality_initializers.bin ]]; then
    out/gpu/fsr-preparation-test.exe --assets fsr4_shaders
fi
