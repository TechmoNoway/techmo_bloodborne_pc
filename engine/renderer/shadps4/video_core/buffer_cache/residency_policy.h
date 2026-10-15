// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/texture_cache/memory_pressure.h"

namespace VideoCore {
struct ResidencyPolicy {
    static constexpr std::uint64_t ChunkBytes = 64ull << 20;
    static bool Fits(std::uint64_t usage, std::uint64_t growth, std::uint64_t bytes,
                     std::uint64_t pressure) {
        return usage < pressure && growth < pressure - usage &&
               bytes < pressure - usage - growth;
    }
    static std::uint32_t IdleSeconds(MemoryPressure::Level level, std::uint32_t normal) {
        return level == MemoryPressure::Level::Critical ? 1 :
               level == MemoryPressure::Level::Pressure ? 5 : normal;
    }
    static std::uint64_t BatchBytes(MemoryPressure::Level level) {
        return (level == MemoryPressure::Level::Critical ? 64ull :
                level == MemoryPressure::Level::Pressure ? 16ull : 8ull) << 20;
    }
    static bool CanEvict(std::uint32_t now, std::uint32_t last_use, std::uint32_t idle,
                         bool gpu_complete, bool gpu_data) {
        return idle != 0 && now >= last_use && now - last_use >= idle &&
               gpu_complete && !gpu_data;
    }
};
} // namespace VideoCore
