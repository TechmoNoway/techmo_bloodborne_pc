// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/texture_cache/gc_policy.h"
#include <cstdio>
#include <cstdint>
#include <limits>
int main() {
    using namespace VideoCore::GcPolicy;
    const auto max=std::numeric_limits<std::uint32_t>::max();
    if (!FullMip(128,64,2,16,128ull*64*2*16) || FullMip(128,64,2,16,128ull*64*2*16-1) ||
        FullMip(0,64,2,16,0) || FullMip(max,max,max,max,16) ||
        !WritebackMipChain(false,16) || WritebackMipChain(false,17) ||
        !WritebackMipChain(true,1) || WritebackMipChain(true,2) || WritebackMipChain(false,0)) return 1;
    std::puts("PASS: mip layout byte coverage, overflow rejection and excluded macro mip tails");
}
