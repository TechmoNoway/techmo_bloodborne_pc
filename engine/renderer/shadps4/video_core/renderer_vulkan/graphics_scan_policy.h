// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace Vulkan::GraphicsScan {
inline constexpr std::uint32_t Budget = 16, Head = 4;
// Preserve early pass coverage and spread the remaining samples through the
// submitted stream, including its final draw. Never increase selection work.
constexpr bool Sample(std::uint32_t ordinal, std::uint32_t total) {
    if (ordinal >= total) return false;
    if (total <= Budget || ordinal < Head) return true;
    constexpr std::uint64_t slots = Budget - Head - 1;
    const std::uint64_t range = total - Head - 1, offset = ordinal - Head;
    const auto slot = (offset * slots + range - 1) / range;
    return slot <= slots && slot * range / slots == offset;
}
}
