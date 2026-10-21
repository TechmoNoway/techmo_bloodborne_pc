// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace VideoCore {
struct IndexBufferRange {
    std::uint64_t address{};
    std::uint32_t size{};
};

// NUM_INDICES need not belong to an indirect draw. Sync at least its declared
// index-buffer capacity as well. Bloodborne's valid capacities fit in 4M indices;
// ignore unbounded/garbage capacity registers rather than requesting a multi-GB upload.
inline std::uint32_t IndirectIndexCount(std::uint32_t draw_count, std::uint32_t capacity) {
    return std::max(draw_count, capacity <= (4u << 20) ? capacity : 0u);
}

// Validate before consulting guest mappings: a null base must stay null even
// with a nonzero offset. BufferCache represents a 40-bit guest address space.
template <typename Clamp>
IndexBufferRange ResolveIndexBufferRange(std::uint64_t base, std::uint32_t offset,
                                        std::uint32_t index_size, std::uint32_t count,
                                        Clamp&& clamp) {
    constexpr std::uint64_t limit = std::uint64_t{1} << 40;
    if (!base || base >= limit || (index_size != 2 && index_size != 4) ||
        base % index_size != 0) {
        return {};
    }
    const auto offset_bytes = std::uint64_t{offset} * index_size;
    if (offset_bytes >= limit - base) {
        return {};
    }
    const auto address = base + offset_bytes;
    const auto requested = std::min({std::uint64_t{count} * index_size, limit - address,
        std::uint64_t{std::numeric_limits<std::uint32_t>::max()}}) / index_size * index_size;
    if (!requested) {
        return {};
    }
    const auto mapped = std::min<std::uint64_t>(clamp(address, requested), requested);
    return {address, static_cast<std::uint32_t>(mapped / index_size * index_size)};
}
} // namespace VideoCore
