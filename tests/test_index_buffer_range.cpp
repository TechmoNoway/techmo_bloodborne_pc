// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <crtdbg.h>
#include "video_core/buffer_cache/index_buffer_range.h"

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    using VideoCore::ResolveIndexBufferRange;
    unsigned calls = 0;
    std::uint64_t seen = 0, requested = 0, mapped = 1000;
    auto clamp = [&](std::uint64_t address, std::uint64_t size) {
        ++calls; seen = address; requested = size; return mapped;
    };
    constexpr std::uint64_t base = 0x1000000000ull, limit = 1ull << 40;
    assert(ResolveIndexBufferRange(0, 100, 2, 100, clamp).size == 0);
    assert(ResolveIndexBufferRange(base, 0, 2, 0, clamp).size == 0);
    assert(ResolveIndexBufferRange(base + 1, 0, 2, 100, clamp).size == 0);
    assert(ResolveIndexBufferRange(limit, 0, 4, 100, clamp).size == 0);
    assert(ResolveIndexBufferRange(limit - 4, 1, 4, 100, clamp).size == 0);
    assert(calls == 0);
    auto range = ResolveIndexBufferRange(base, 3, 2, 10, clamp);
    assert(range.address == base + 6 && range.size == 20 && requested == 20);
    mapped = 19;
    assert(ResolveIndexBufferRange(base, 3, 2, 10, clamp).size == 18);
    assert(ResolveIndexBufferRange(base, 3, 4, 10, clamp).size == 16);
    mapped = 1;
    assert(ResolveIndexBufferRange(base, 0, 4, 10, clamp).size == 0);
    mapped = 0;
    assert(ResolveIndexBufferRange(base, 0, 2, 10, clamp).size == 0);
    mapped = ~0ull;
    range = ResolveIndexBufferRange(base, 0xffffffffu, 4, 0xffffffffu, clamp);
    assert(seen == base + 0x3fffffffcull && range.size == 0xfffffffcu);
    assert(requested == 0xfffffffcu);
    range = ResolveIndexBufferRange(limit - 8, 0, 4, 0xffffffffu, clamp);
    assert(range.size == 8 && requested == 8);
    puts("PASS: null/unmapped bases, partial mappings, 16/32-bit alignment, wide offsets and count limits");
}
