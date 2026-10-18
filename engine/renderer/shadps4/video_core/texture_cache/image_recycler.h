// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/texture_cache/image.h"
#include "bbport_toggles.h"
#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace VideoCore {
// Only retired, ordinary CPU-backed texture allocations enter this pool. Views,
// hashes, guest identity and layout state never carry over to the next owner.
class ImageRecycler {
public:
    using Clock = std::chrono::steady_clock;
    explicit ImageRecycler(u64 maximum = ConfiguredLimit()) : maximum{maximum} {}
    static u64 ConfiguredLimit() {
        const char* value = std::getenv("BB_IMAGE_RECYCLE_MB");
        return std::min<u64>(value ? std::strtoull(value, nullptr, 10) : 16, 64) << 20;
    }
    void Budget(u64 usage, u64 budget, bool available, Clock::time_point now = Clock::now()) {
        std::scoped_lock lock{mutex};
        limit = available && budget && usage < budget / 4 * 3 ? std::min(maximum, budget / 256) : 0;
        while (!entries.empty() && (bytes > limit || now - entries.front().retired >= std::chrono::seconds(1)))
            DropFront();
    }
    bool Acquire(const vk::ImageCreateInfo& requested, UniqueImage& result) {
        if (!Eligible(requested)) return false;
        std::scoped_lock lock{mutex};
        const auto now = Clock::now();
        while (!entries.empty() && now - entries.front().retired >= std::chrono::seconds(1)) DropFront();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (Same(requested, it->image.image_ci)) {
                const auto size = it->image.size_bytes;
                result = std::move(it->image);
                entries.erase(it);
                bytes -= size;
                BbStats::image_recycle_bytes.fetch_sub(size, std::memory_order_relaxed);
                BbStats::image_recycle_hits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }
    // Caller must have retired every recorded GPU use and removed all views.
    void Retire(UniqueImage image) {
        if (!image || !Eligible(image.image_ci)) return;
        std::scoped_lock lock{mutex};
        const auto size = image.size_bytes;
        if (!limit || size > limit) return;
        while (!entries.empty() && (entries.size() >= 64 || size > limit - bytes)) DropFront();
        // Retention is optional: allocation pressure must fall back to normal
        // destruction, never turn a completed GPU retirement into a failure.
        try { entries.push_back({std::move(image), Clock::now()}); }
        catch (const std::bad_alloc&) { return; }
        bytes += size;
        BbStats::image_recycle_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    ~ImageRecycler() {
        BbStats::image_recycle_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
    u64 Bytes() const { std::scoped_lock lock{mutex}; return bytes; }
private:
    static bool Eligible(const vk::ImageCreateInfo& ci) {
        return !ci.pNext && ci.sharingMode == vk::SharingMode::eExclusive && !ci.queueFamilyIndexCount &&
               ci.initialLayout == vk::ImageLayout::eUndefined;
    }
    static bool Same(const vk::ImageCreateInfo& a, const vk::ImageCreateInfo& b) {
        return a.flags == b.flags && a.imageType == b.imageType && a.format == b.format &&
               a.extent == b.extent && a.mipLevels == b.mipLevels && a.arrayLayers == b.arrayLayers &&
               a.samples == b.samples && a.tiling == b.tiling && a.usage == b.usage;
    }
    struct Entry { UniqueImage image; Clock::time_point retired; };
    void DropFront() {
        const auto size = entries.front().image.size_bytes;
        entries.pop_front(); bytes -= size;
        BbStats::image_recycle_bytes.fetch_sub(size, std::memory_order_relaxed);
    }
    const u64 maximum;
    u64 limit = 0, bytes = 0;
    mutable std::mutex mutex;
    std::deque<Entry> entries;
};
} // namespace VideoCore
