// SPDX-License-Identifier: GPL-2.0-or-later
// Oversized deferred payloads must retain their contents until Vulkan records them.
// Adapted from DarkIzuku dceac3f478; additional cancellation/exception coverage is local.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <span>
#include <vector>
#include <limits>
#include <cassert>
#include <cstdio>
#include <crtdbg.h>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _putenv_s("BB_PIPELINE_CACHE", "0");
    _putenv_s("BB_NULL_GPU", "1");
    _putenv_s("BB_PRODUCER_CHECK", "1");
    // CPU-side bounds and over-aligned data before the actual Vulkan readback.
    {
        Vulkan::RecordChunk chunk;
        assert(!chunk.ReserveData(std::numeric_limits<size_t>::max()));
        assert(!chunk.Allocate(1, 0) && !chunk.Allocate(1, 3));
        assert(!chunk.Allocate(std::numeric_limits<size_t>::max(), 64));
        assert(chunk.ReserveData(64 * 1024 + 4096));
        auto *aligned = chunk.Allocate(64 * 1024, 4096);
        assert(aligned && reinterpret_cast<uintptr_t>(aligned) % 4096 == 0);
        chunk.Execute({});
        assert(chunk.Size() == 0 && chunk.Empty());
        assert(chunk.ReserveData(300 * 1024 + 4096));
        auto *spilled = chunk.Allocate(300 * 1024, 4096);
        assert(spilled && reinterpret_cast<uintptr_t>(spilled) % 4096 == 0);
        std::memset(spilled, 0x5a, 300 * 1024);
        assert(chunk.Push([spilled](vk::CommandBuffer) {
            const auto *bytes = static_cast<unsigned char *>(spilled);
            for (size_t i = 0; i < 300 * 1024; ++i)
                assert(bytes[i] == 0x5a);
        }));
        chunk.Execute({});
        assert(chunk.Size() == 0 && chunk.Empty());
    }
    // Dropped chunks release command captures without executing them, including
    // after Execute throws. Spill storage stays owned until the chunk is reset.
    for (bool throws : {false, true}) {
        std::weak_ptr<int> weak;
        {
            Vulkan::RecordChunk chunk;
            auto owner = std::make_shared<int>(42);
            weak = owner;
            assert(chunk.ReserveData(300 * 1024));
            auto *data = static_cast<unsigned char *>(chunk.Allocate(300 * 1024, 1));
            assert(data);
            data[300 * 1024 - 1] = 0x5a;
            assert(chunk.Push([owner, data](vk::CommandBuffer) {
                assert(*owner == 42 && data[300 * 1024 - 1] == 0x5a);
                throw std::runtime_error("recording fixture");
            }));
            owner.reset();
            assert(!weak.expired());
            if (throws) {
                bool caught = false;
                try {
                    chunk.Execute({});
                } catch (const std::runtime_error &) {
                    caught = true;
                }
                assert(caught);
            }
        }
        assert(weak.expired());
    }
    if (argc == 2 && !std::strcmp(argv[1], "--cpu-only")) {
        std::puts("PASS: RecordChunk oversized data, alignment, arithmetic and recycling (CPU)");
        return 0;
    }
    Vulkan::Instance instance(-1, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    // Explicit entry points keep fixture/production Vulkan-Hpp initialization consistent.
    const auto get_instance =
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance(static_cast<VkInstance>(instance.GetInstance()), "vkGetDeviceProcAddr"));
    d.vkCmdCopyBuffer = reinterpret_cast<PFN_vkCmdCopyBuffer>(
        get_device(static_cast<VkDevice>(instance.GetDevice()), "vkCmdCopyBuffer"));
    d.vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
        get_device(static_cast<VkDevice>(instance.GetDevice()), "vkCmdPipelineBarrier2"));
    assert(d.vkCmdCopyBuffer && d.vkCmdPipelineBarrier2);
    Vulkan::Scheduler scheduler(instance, true);
    assert(scheduler.IsRecordingDeferred());
    // More regions than can fit in the former fixed 128 KiB recording chunk.
    constexpr size_t Count = Vulkan::RecordChunk::Capacity / sizeof(vk::BufferCopy) + 257;
    VkBuffer buffers[2]{};
    VmaAllocation allocations[2]{};
    VmaAllocationInfo maps[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = Count * sizeof(u32),
                                    .usage = i ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                                               : VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        const VmaAllocationCreateInfo ai{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                                  VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                         .usage = VMA_MEMORY_USAGE_AUTO};
        assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ai, &buffers[i], &allocations[i],
                               &maps[i]) == VK_SUCCESS);
    }
    auto *source = static_cast<u32 *>(maps[0].pMappedData);
    auto *output = static_cast<u32 *>(maps[1].pMappedData);
    std::atomic<unsigned> executed{0};
    struct alignas(64) Marker {
        u32 value;
    };
    // Direct-mode RecordData is a borrowed span and must not allocate or copy.
    scheduler.CommandBuffer();
    const u32 direct_value = 123;
    assert(scheduler.RecordData(std::span{&direct_value, 1}).data() == &direct_value);
    scheduler.KickRecording(true);
    assert(scheduler.IsRecordingDeferred());
    for (unsigned pass = 0; pass < 12; ++pass) {
        for (size_t i = 0; i < Count; ++i)
            source[i] = u32(i) ^ ((pass + 1) * 0x1234567u);
        assert(vmaFlushAllocation(instance.GetAllocator(), allocations[0], 0, VK_WHOLE_SIZE) ==
               VK_SUCCESS);
        std::vector<vk::BufferCopy> regions(Count);
        for (size_t i = 0; i < Count; ++i)
            regions[i] = {
                .srcOffset = i * sizeof(u32), .dstOffset = i * sizeof(u32), .size = sizeof(u32)};
        // Start near a chunk boundary so a reservation must also preserve earlier commands.
        for (unsigned i = 0; i < 4000; ++i)
            scheduler.Record([](vk::CommandBuffer) {});
        if (pass % 2 == 0) {
            const auto saved = scheduler.RecordData(std::span<const vk::BufferCopy>{regions});
            scheduler.Record([saved, src = vk::Buffer{buffers[0]}, dst = vk::Buffer{buffers[1]}, &d,
                              &executed](vk::CommandBuffer cmd) {
                cmd.copyBuffer(src, dst, saved.size(), saved.data(), d);
                executed.fetch_add(1);
            });
        } else {
            // Individually fitting arrays whose combined reservation exceeds the chunk.
            const auto half = Count / 2;
            scheduler.ReserveRecordData(regions.size() * sizeof(regions[0]) + sizeof(Marker) + 256);
            const auto first =
                scheduler.RecordData(std::span<const vk::BufferCopy>{regions.data(), half});
            const Marker marker{.value = pass};
            const auto aligned = scheduler.RecordData(std::span<const Marker>{&marker, 1});
            const auto second = scheduler.RecordData(
                std::span<const vk::BufferCopy>{regions.data() + half, Count - half});
            assert(reinterpret_cast<uintptr_t>(aligned.data()) % alignof(Marker) == 0);
            scheduler.Record([first, second, aligned, pass, src = vk::Buffer{buffers[0]},
                              dst = vk::Buffer{buffers[1]}, &d, &executed](vk::CommandBuffer cmd) {
                assert(aligned[0].value == pass);
                cmd.copyBuffer(src, dst, first.size(), first.data(), d);
                cmd.copyBuffer(src, dst, second.size(), second.data(), d);
                executed.fetch_add(1);
            });
        }
        // Source descriptors are no longer valid; the recorder must own its copies.
        std::memset(regions.data(), 0, regions.size() * sizeof(regions[0]));
        regions.clear();
        regions.shrink_to_fit();
        scheduler.KickRecording(); // Exercise chunk handoff and segment ownership too.
        for (unsigned i = 0; i < 9000; ++i)
            scheduler.Record([](vk::CommandBuffer) {});
        scheduler.KickRecording();
        scheduler.Record([&d](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                             .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                             .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                             .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, d);
        });
        scheduler.Finish();
        assert(vmaInvalidateAllocation(instance.GetAllocator(), allocations[1], 0, VK_WHOLE_SIZE) ==
               VK_SUCCESS);
        assert(std::memcmp(source, output, Count * sizeof(u32)) == 0);
        assert(executed.load() == pass + 1);
    }
    for (unsigned i = 0; i < 2; ++i)
        vmaDestroyBuffer(instance.GetAllocator(), buffers[i], allocations[i]);
    std::puts("PASS: oversized and aggregate record data, alignment, source lifetime, chunk "
              "recycling and Vulkan readback");
}
