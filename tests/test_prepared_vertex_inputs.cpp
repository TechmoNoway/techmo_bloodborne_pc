// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <crtdbg.h>
#include "video_core/renderer_vulkan/prepared_vertex_inputs.h"

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    using Descriptor = std::array<unsigned, 4>; // Four owned descriptor dwords, as in a V#.
    std::array<Descriptor, 2> prepared{{{0x1000, 16, 24, 0x123}, {0x2000, 32, 48, 0x456}}};
    auto packet = prepared;
    auto matches = [&] {
        return Vulkan::PreparedVertexMatchesPacket<Descriptor>(prepared, packet);
    };
    assert(matches());
    // Simulate a guest rewrite after worker preparation: address, stride, count and format.
    for (unsigned descriptor = 0; descriptor < packet.size(); ++descriptor) {
        for (unsigned word = 0; word < packet[descriptor].size(); ++word) {
            packet[descriptor][word] ^= 1;
            assert(!matches());
            packet[descriptor][word] ^= 1;
        }
    }
    assert(!Vulkan::PreparedVertexMatchesPacket<Descriptor>(prepared, std::span{packet}.first(1)));
    assert(!Vulkan::PreparedVertexMatchesPacket<Descriptor>(std::span{prepared}.first(1), packet));
    assert(Vulkan::PreparedVertexMatchesPacket<Descriptor>(prepared, {}));
    puts("PASS: matching V#s, worker/packet address-stride-format rewrites, counts and direct path");
}
