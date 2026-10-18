// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_image_hash.h"
#include "bbport_copy.h"
#include "bbport_platform.h"
#include "bbport_toggles.h"
#include <vector>
#include <new>
#include <xxhash.h>
namespace BbImageHash {
std::uint64_t Serial(const void* data, std::size_t size) {
    BbStats::Timer timer{BbStats::t_image_hash};
    const auto hash = XXH3_64bits(data, size);
    BbStats::image_hash_bytes.fetch_add(size, std::memory_order_relaxed);
    return hash;
}
bool SnapshotParallel(const void* data, std::size_t size, std::span<const Range> mips,
                      std::span<std::uint64_t> output) {
    if (size < (2u << 20) || size > (16u << 20) || mips.size() < 2 ||
        mips.size() > 16 || output.size() != mips.size() || !BbCopy::Enabled()) return false;
    std::size_t hashed = 0;
    for (const auto& mip : mips) {
        if (!mip.size || mip.offset > size || mip.size > size - mip.offset) return false;
        hashed += mip.size;
    }
    thread_local std::vector<std::uint8_t> snapshot;
    {
        BbStats::Timer timer{BbStats::t_hash_snapshot};
        try { snapshot.resize(size); }
        catch (const std::bad_alloc&) { return false; }
        if (!BbPlatform::ReadProcessMemory(data, snapshot.data(), size)) return false;
    }
    {
        BbStats::Timer timer{BbStats::t_image_hash};
        const auto* bytes = snapshot.data();
        BbCopy::ParallelFor(mips.size(), [&](std::size_t index) {
            output[index] = XXH3_64bits(bytes + mips[index].offset, mips[index].size);
        });
    }
    BbStats::image_hash_bytes.fetch_add(hashed, std::memory_order_relaxed);
    BbStats::image_hash_parallel_jobs.fetch_add(mips.size(), std::memory_order_relaxed);
    return true;
}
}
