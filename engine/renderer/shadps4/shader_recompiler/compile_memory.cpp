// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/compile_memory.h"

namespace Shader {
bool CompileMemory::ReadRegister(u32 base, u32 offset, void* data, size_t size) const {
    if (!replay) return false;
    for (const auto& read : reads) {
        if (read.base == base && read.offset == offset && read.bytes.size() == size) {
            std::memcpy(data, read.bytes.data(), size);
            return true;
        }
    }
    throw std::runtime_error("shader replay needs uncaptured register data");
}

void CompileMemory::CaptureRegister(u32 base, u32 offset, const void* data, size_t size) {
    for (const auto& read : reads) {
        if (read.base == base && read.offset == offset && read.bytes.size() == size) {
            if (std::memcmp(read.bytes.data(), data, size)) valid = false;
            return;
        }
    }
    if (reads.size() >= 512 || size > 4096) {
        valid = false;
        return;
    }
    const auto* bytes = static_cast<const u8*>(data);
    reads.push_back({base, offset, {bytes, bytes + size}});
}
} // namespace Shader
