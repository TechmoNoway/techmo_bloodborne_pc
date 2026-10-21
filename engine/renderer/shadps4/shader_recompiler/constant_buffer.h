// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <string_view>
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
namespace Shader {
inline bool NativeConstantsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_NATIVE_CONSTANTS");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}
// A bounded std140 uvec4 array fits the Vulkan core minimum uniform range (16 KiB).
inline bool UniformConstants(const Profile& profile, const Info& info) {
    if (!profile.native_constants || !info.srt_info.flattened_bufsize_dw ||
        info.srt_info.flattened_bufsize_dw > 4096) return false;
    for (const auto& buffer : info.buffers)
        if (buffer.buffer_type == BufferType::Flatbuf)
            return !buffer.is_written && buffer.used_types == IR::Type::U32;
    return false;
}
} // namespace Shader
