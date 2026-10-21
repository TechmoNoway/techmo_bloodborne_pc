// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
#include <span>
#include <type_traits>

namespace Vulkan {
// An absent packet is the direct path. Otherwise its owned V# snapshot is authoritative,
// including the count: preparation may have read the guest table before it was rewritten.
template <typename Descriptor>
bool PreparedVertexMatchesPacket(std::span<const Descriptor> prepared,
                                 std::span<const Descriptor> packet) {
    static_assert(std::is_trivially_copyable_v<Descriptor>);
    return packet.empty() ||
           (prepared.size() == packet.size() &&
            std::memcmp(prepared.data(), packet.data(), packet.size_bytes()) == 0);
}
} // namespace Vulkan
