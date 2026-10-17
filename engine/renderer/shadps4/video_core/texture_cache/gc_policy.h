// SPDX-License-Identifier: GPL-3.0-or-later
// Layout guards adapted from DarkIzuku dceac3f478; budget policy remains Techmo's.
#pragma once
#include <cstdint>
#include <initializer_list>
namespace VideoCore::GcPolicy {
constexpr bool FullMip(std::uint32_t pitch, std::uint32_t height, std::uint32_t layers,
                       std::uint32_t bytes, std::uint64_t size) {
    if (!pitch || !height || !layers || !bytes) return false;
    for (const auto factor : {pitch,height,layers,bytes}) {
        if (size % factor) return false;
        size /= factor;
    }
    return size == 1;
}
constexpr bool WritebackMipChain(bool macro_tiled, std::uint32_t levels) {
    return levels && levels <= 16 && (!macro_tiled || levels == 1);
}
}
