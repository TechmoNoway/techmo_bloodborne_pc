// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <vulkan/vulkan_core.h>

namespace Vulkan {

// One cache per logical device. This is only the size used by our dedicated
// allocation heuristic: VMA still queries and binds each actual image normally.
class ImageAllocationSizeCache {
public:
    struct Result { VkDeviceSize size; bool hit; bool cacheable; };
    explicit ImageAllocationSizeCache(size_t capacity = 512) : capacity{capacity} {}

    template <typename Query>
    Result Get(const VkImageCreateInfo& ci, Query&& query) {
        // Extension chains may carry format lists, external-memory handles, etc.
        // Do not approximate their identity or retain caller-owned pointers.
        if (!capacity || ci.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || ci.pNext ||
            ci.sharingMode != VK_SHARING_MODE_EXCLUSIVE || ci.queueFamilyIndexCount ||
            (ci.flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                         VK_IMAGE_CREATE_SPARSE_ALIASED_BIT | VK_IMAGE_CREATE_DISJOINT_BIT))) {
            return {query(), false, false};
        }
        const Key key{ci.flags, uint32_t(ci.imageType), uint32_t(ci.format), ci.extent.width,
            ci.extent.height, ci.extent.depth, ci.mipLevels, ci.arrayLayers, uint32_t(ci.samples),
            uint32_t(ci.tiling), ci.usage, uint32_t(ci.initialLayout)};
        {
            std::scoped_lock lock{mutex};
            if (const auto it = entries.find(key); it != entries.end()) {
                it->second.used = NextTick();
                return {it->second.size, true, true};
            }
        }
        // A slow driver call for one shape must not hold up unrelated cache hits.
        // Concurrent first misses may query twice; neither result changes a live allocation.
        const VkDeviceSize size = query();
        if (size) {
            std::scoped_lock lock{mutex};
            try {
                if (entries.size() >= capacity && !entries.contains(key)) {
                    const auto oldest = std::min_element(entries.begin(), entries.end(),
                        [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                    entries.erase(oldest);
                }
                entries.insert_or_assign(key, Entry{size, NextTick()});
            } catch (const std::bad_alloc&) {
                // Optional CPU caching must not turn an otherwise valid allocation into failure.
            }
        }
        return {size, false, true};
    }
    size_t Size() const { std::scoped_lock lock{mutex}; return entries.size(); }

private:
    using Key = std::array<uint32_t, 12>;
    struct Entry { VkDeviceSize size; uint64_t used; };
    uint64_t NextTick() {
        if (tick == std::numeric_limits<uint64_t>::max()) {
            // Ordering only affects eviction, never image identity or query values.
            for (auto& [key, entry] : entries) entry.used = 0;
            tick = 0;
        }
        return ++tick;
    }
    const size_t capacity;
    mutable std::mutex mutex;
    std::map<Key, Entry> entries;
    uint64_t tick = 0;
};
} // namespace Vulkan
