// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <crtdbg.h>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>
#include "video_core/renderer_vulkan/image_allocation_size_cache.h"

using Vulkan::ImageAllocationSizeCache;
static VkImageCreateInfo Image() {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {128, 128, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return ci;
}
int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    auto ci = Image();
    ImageAllocationSizeCache cache;
    unsigned calls = 0;
    const auto query = [&] { return VkDeviceSize{++calls * 4096}; };
    assert(cache.Get(ci, query).size == 4096);
    const auto cached = cache.Get(ci, query);
    assert(cached.hit && cached.cacheable && cached.size == 4096 && calls == 1);
    // Every create-info field in the supported domain changes the key.
    for (unsigned field = 0; field < 12; ++field) {
        auto changed = ci;
        switch (field) {
        case 0: changed.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; break;
        case 1: changed.imageType = VK_IMAGE_TYPE_3D; break;
        case 2: changed.format = VK_FORMAT_B8G8R8A8_UNORM; break;
        case 3: ++changed.extent.width; break;
        case 4: ++changed.extent.height; break;
        case 5: ++changed.extent.depth; break;
        case 6: ++changed.mipLevels; break;
        case 7: ++changed.arrayLayers; break;
        case 8: changed.samples = VK_SAMPLE_COUNT_2_BIT; break;
        case 9: changed.tiling = VK_IMAGE_TILING_LINEAR; break;
        case 10: changed.usage |= VK_IMAGE_USAGE_STORAGE_BIT; break;
        case 11: changed.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED; break;
        }
        const auto fresh = cache.Get(changed, query);
        assert(!fresh.hit && fresh.cacheable && fresh.size != cached.size);
        assert(cache.Get(changed, query).hit);
    }
    const auto count = cache.Size();
    for (unsigned field = 0; field < 5; ++field) {
        auto unsupported = ci;
        switch (field) {
        case 0: unsupported.pNext = &ci; break; // Not dereferenced or retained.
        case 1: unsupported.sharingMode = VK_SHARING_MODE_CONCURRENT; break;
        case 2: unsupported.queueFamilyIndexCount = 2; break;
        case 3: unsupported.flags = VK_IMAGE_CREATE_DISJOINT_BIT; break;
        case 4: unsupported.flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT; break;
        }
        const auto first = cache.Get(unsupported, query), second = cache.Get(unsupported, query);
        assert(!first.hit && !first.cacheable && !second.hit && first.size != second.size);
    }
    assert(cache.Size() == count);
    ImageAllocationSizeCache other;
    assert(other.Get(ci, [] { return VkDeviceSize{123}; }).size == 123);
    assert(cache.Get(ci, query).size == 4096); // Device owners never share entries.
    ImageAllocationSizeCache disabled{0};
    assert(!disabled.Get(ci, query).cacheable && !disabled.Get(ci, query).hit && disabled.Size() == 0);
    ImageAllocationSizeCache retry;
    assert(retry.Get(ci, [] { return VkDeviceSize{0}; }).size == 0 && retry.Size() == 0);
    try { retry.Get(ci, []() -> VkDeviceSize { throw std::runtime_error("driver failure"); }); }
    catch (const std::runtime_error&) {}
    assert(retry.Size() == 0 && retry.Get(ci, query).size != 0);

    ImageAllocationSizeCache bounded{2};
    auto b = ci, c = ci; b.extent.width = 256; c.extent.width = 512;
    bounded.Get(ci, query); bounded.Get(b, query); bounded.Get(ci, query); bounded.Get(c, query);
    assert(bounded.Size() == 2 && bounded.Get(ci, query).hit && !bounded.Get(b, query).hit);

    // A cold driver query cannot lock out an already cached hot shape.
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto slow = std::async(std::launch::async, [&] {
        return cache.Get(b, [&] { entered.set_value(); released.wait(); return VkDeviceSize{65536}; });
    });
    entered.get_future().wait();
    auto hot = std::async(std::launch::async, [&] { return cache.Get(ci, [] { std::abort(); return VkDeviceSize{}; }); });
    const bool unblocked = hot.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release.set_value();
    assert(unblocked && hot.get().hit && slow.get().size == 65536);
    std::atomic<unsigned> unexpected{0};
    std::vector<std::thread> threads;
    for (unsigned n = 0; n < 8; ++n) threads.emplace_back([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            const auto result = cache.Get(ci, [&] { ++unexpected; return VkDeviceSize{1}; });
            assert(result.hit && result.size == 4096);
        }
    });
    for (auto& thread : threads) thread.join();
    assert(unexpected == 0);
    puts("PASS: full image identity, extension/sharing bypass, device isolation, bounded LRU, failure retry and concurrent nonblocking hits");
}
