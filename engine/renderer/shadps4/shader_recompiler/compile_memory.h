// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/frontend/tessellation.h"

namespace Shader {
// Only attached while compiling a new permutation. Replay never dereferences addresses
// from a previous game process. A compiler which requests different inputs falls back.
struct CompileMemory {
    struct Read {
        u32 base{}, offset{};
        std::vector<u8> bytes;
    };
    bool replay = false;
    bool valid = true;
    std::vector<Read> reads;
    std::vector<u32> flat;
    std::vector<u8> walker;
    std::vector<u8> fetch;
    std::optional<TessellationDataConstantBuffer> tess;

    bool ReadRegister(u32 base, u32 offset, void* data, size_t size) const;
    void CaptureRegister(u32 base, u32 offset, const void* data, size_t size);
};
} // namespace Shader
