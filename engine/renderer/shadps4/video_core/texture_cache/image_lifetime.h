// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
namespace VideoCore::ImageLifetime {
// Independent of collector frequency, frame cap and GPU submission count.
constexpr bool CanCollect(std::uint64_t use, std::uint64_t current, bool completed,
                          std::uint64_t used_ms, std::uint64_t now_ms) {
    if (use && (use>=current || !completed)) return false;
    return !used_ms || (now_ms>=used_ms && now_ms-used_ms>=250);
}
}
