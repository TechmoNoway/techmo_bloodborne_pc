// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/amdgpu/regs_shader.h"
#include <cstring>
#include <optional>

namespace Vulkan {
// Speculative register replay may see a shader before its binary header is
// available. Unlike the required lookup, a missing header is not an assertion.
// The caller guards guest reads with runtime_fault_recover.
template <typename Program>
std::optional<Shader::ShaderParams> TryPreparationParams(const Program &program) {
    const auto *code = program.template Address<const u32 *>();
    if (!code)
        return std::nullopt;
    constexpr size_t SearchWords = 0x4000;
    constexpr size_t HeaderWords = (sizeof(AmdGpu::BinaryInfo) + 3) / 4;
    const auto read = [&](size_t offset) -> std::optional<Shader::ShaderParams> {
        AmdGpu::BinaryInfo info;
        std::memcpy(&info, code + offset, sizeof(info));
        if (!info.Valid() || !info.length || info.length % sizeof(u32))
            return std::nullopt;
        return Shader::ShaderParams{.user_data = program.user_data,
                                    .code = {code, info.length / sizeof(u32)},
                                    .hash = info.shader_hash};
    };
    if (code[0] == 0xBEEB03FF) {
        const u64 offset = (u64(code[1]) + 1) * 2;
        if (offset <= SearchWords - HeaderWords)
            if (auto result = read(offset))
                return result;
    }
    for (size_t offset = 0; offset <= SearchWords - HeaderWords; ++offset)
        if (auto result = read(offset))
            return result;
    return std::nullopt;
}
} // namespace Vulkan
