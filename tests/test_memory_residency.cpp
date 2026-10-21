// SPDX-License-Identifier: GPL-2.0-or-later
// Production Vulkan image allocation and GPU-deferred reclamation; no game/window.
#include <cassert>
#include <array>
#include <cstdio>
#include <memory>
#include <vector>
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

int main() {
    std::fprintf(stderr, "Memory test: initializing Vulkan\n");
    Vulkan::Instance instance{-1, false};
    std::fprintf(stderr, "Memory test: initializing scheduler\n");
    Vulkan::Scheduler scheduler{instance, false};
    static vk::detail::DynamicLoader loader;
    // Do not instantiate Vulkan-Hpp's init() here: its platform-dependent inline
    // definitions can be coalesced with vk_platform.cpp in this static/LTO link.
    const auto get_instance_proc = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance_proc(static_cast<VkInstance>(instance.GetInstance()), "vkGetDeviceProcAddr"));
    const auto barrier_fn = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
        get_device_proc(static_cast<VkDevice>(instance.GetDevice()), "vkCmdPipelineBarrier2"));
    const auto clear_fn = reinterpret_cast<PFN_vkCmdClearColorImage>(
        get_device_proc(static_cast<VkDevice>(instance.GetDevice()), "vkCmdClearColorImage"));
    const auto requirements_fn = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
        get_device_proc(static_cast<VkDevice>(instance.GetDevice()), "vkGetImageMemoryRequirements"));
    assert(barrier_fn && clear_fn && requirements_fn);

    const auto baseline = BbStats::vk_image_bytes.load();
    const auto before = instance.GetDeviceMemorySnapshot();
    std::fprintf(stderr, "Memory test: allocating images\n");
    std::vector<std::unique_ptr<VideoCore::UniqueImage>> images;
    const vk::ImageCreateInfo ci{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {1024, 1024, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
    };
    u64 allocated = 0;
    bool hit = false, cacheable = false;
    const auto hint = instance.GetImageAllocationSize(ci, hit, cacheable);
    const bool cache_enabled = !(std::getenv("BB_IMAGE_REQUIREMENTS_CACHE") &&
                                std::string_view{std::getenv("BB_IMAGE_REQUIREMENTS_CACHE")} == "0");
    assert(!hit && cacheable == cache_enabled);
    assert(instance.GetImageAllocationSize(ci, hit, cacheable) == hint && hit == cache_enabled);
    const auto query_hits_before = BbStats::image_query_hits.load();
    const auto query_bypass_before = BbStats::image_query_bypasses.load();
    for (unsigned n = 0; n < 8; ++n) {
        auto image = std::make_unique<VideoCore::UniqueImage>(instance);
        image->Create(ci);
        VkMemoryRequirements actual{};
        requirements_fn(static_cast<VkDevice>(instance.GetDevice()), static_cast<VkImage>(image->image), &actual);
        assert(actual.size == hint);
        if (n == 0) {
            VideoCore::UniqueImage moved{std::move(*image)};
            assert(moved.instance == &instance && image->instance == nullptr);
            *image = std::move(moved);
            assert(image->instance == &instance && moved.instance == nullptr);
        }
        std::fprintf(stderr, "Memory test: recording image %u\n", n);
        allocated += image->size_bytes;
        const auto cmd = scheduler.CommandBuffer();
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image->image, .subresourceRange = range,
        };
        const VkImageMemoryBarrier2 raw_barrier = static_cast<VkImageMemoryBarrier2>(barrier);
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &raw_barrier};
        barrier_fn(static_cast<VkCommandBuffer>(cmd), &dependency);
        const VkClearColorValue color{{.25f, .5f, .75f, 1.f}};
        const VkImageSubresourceRange raw_range = static_cast<VkImageSubresourceRange>(range);
        clear_fn(static_cast<VkCommandBuffer>(cmd), static_cast<VkImage>(image->image),
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &raw_range);
        images.push_back(std::move(image));
    }
    if (BbStats::enabled) {
        const auto hits = BbStats::image_query_hits.load() - query_hits_before;
        const auto bypasses = BbStats::image_query_bypasses.load() - query_bypass_before;
        const char* threshold = std::getenv("BB_VMA_DEDICATED_MB");
        const bool queries_enabled = !threshold || std::strtoull(threshold, nullptr, 10) != 0;
        assert(hits == (queries_enabled && cache_enabled ? 8u : 0u));
        assert(bypasses == (queries_enabled && !cache_enabled ? 8u : 0u));
        std::printf("Image query counters: hits=%llu bypasses=%llu\n",
                    (unsigned long long)hits, (unsigned long long)bypasses);
    }
    assert(allocated >= 8ull * 1024 * 1024 * 4);
    assert(BbStats::vk_image_bytes.load() == baseline + allocated);
    const auto live = instance.GetDeviceMemorySnapshot();
    bool retired = false;
    scheduler.DeferOperation([&] { images.clear(); retired = true; });
    assert(!retired && BbStats::vk_image_bytes.load() == baseline + allocated);
    scheduler.Finish();
    std::fprintf(stderr, "Memory test: submitted work complete\n");
    scheduler.PopPendingOperations();
    assert(retired && images.empty() && BbStats::vk_image_bytes.load() == baseline);
    const auto freed = instance.GetDeviceMemorySnapshot();
    if (instance.CanReportMemoryUsage()) {
        assert(before.budget > 0 && live.budget > 0 && freed.budget > 0);
        // Driver estimates can update asynchronously. No asserted VRAM saving or total
        // budget stability based on this short synthetic workload.
        std::printf("Driver usage before/live/freed: %llu / %llu / %llu bytes; tracked images: %llu -> 0\n",
                    (unsigned long long)before.usage, (unsigned long long)live.usage,
                    (unsigned long long)freed.usage, (unsigned long long)allocated);
    }
    puts("PASS: cached size matches actual Vulkan requirements, device cache survives image moves, image accounting and GPU-deferred release");
}
