// SPDX-License-Identifier: GPL-2.0-or-later
// Real legacy Windows caches; a small mapped guest fixture, no game/window.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <crtdbg.h>
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/texture_cache.h"
#include "memory_safety_comp.h"
#include "null_index_vert.h"

// Replace only the guest-address services in the shared fail-closed GPU stubs.
#define runtime_memory_region budget_unused_region
#define runtime_memory_clamp budget_unused_clamp
#define runtime_memory_set_write_watch budget_unused_watch
#define runtime_memory_gpu_protect budget_unused_protect
#define runtime_memory_read_backing budget_unused_read
#define runtime_memory_write_backing budget_unused_write
#include "test_gpu_runtime_stubs.cpp"
#undef runtime_memory_region
#undef runtime_memory_clamp
#undef runtime_memory_set_write_watch
#undef runtime_memory_gpu_protect
#undef runtime_memory_read_backing
#undef runtime_memory_write_backing

constexpr uintptr_t Guest = 0x1000000000ull;
constexpr u64 GuestSize = 128ull << 20, Size = 64ull << 10;
// The fixture has no guest threads or fault handlers. Keep production RegionManager
// ownership bits, but replace the OS page-watcher boundary (not the cache logic).
namespace VideoCore {
struct TextureLookupTest {
    static void Run(TextureCache& cache) {
        auto make = [&](VAddr address, u32 width, u32 height) {
            ImageInfo info{};
            info.guest_address=address; info.guest_size=u64(width)*height*4;
            info.type=AmdGpu::ImageType::Color2D; info.pixel_format=vk::Format::eR8G8B8A8Unorm;
            info.size={width,height,1}; info.pitch=width; info.num_bits=32;
            info.resources={1,1}; info.mips_layout[0]={info.guest_size,width,height,0};
            const auto id=cache.slot_images.insert(cache.instance,cache.runtime,cache.slot_image_views,info);
            cache.RegisterImage(id); return id;
        };
        constexpr VAddr base=Guest+(16ull<<20);
        const auto large=make(base,4096,2048); // Spans 32 registry pages.
        const auto small=make(base,128,128);
        const auto adjacent=make(base+(1ull<<20),128,128);
        assert(cache.FindImageFromRange(base,32ull<<20,false)==large);
        assert(cache.FindImageFromRange(base,64ull<<10,false)==small);
        assert(!cache.FindImageFromRange(base,0,false));
        assert(!cache.FindImageFromRange(base+256,64ull<<10,false));
        cache.slot_images[large].flags=ImageFlagBits::Registered|ImageFlagBits::GpuModified;
        assert(cache.FindImageFromRange(base,32ull<<20,true)==large);
        assert(!cache.FindImageFromRange(base+(1ull<<20),64ull<<10,true));
        cache.UnregisterImage(small);
        assert(cache.FindImageFromRange(base,64ull<<10,false)==large); // Single candidate fallback retained.
        cache.UnregisterImage(large);
        assert(!cache.FindImageFromRange(base,32ull<<20,false));
        assert(cache.FindImageFromRange(base+(1ull<<20),64ull<<10,false)==adjacent);
        cache.UnregisterImage(adjacent);
        cache.slot_images.erase(large); cache.slot_images.erase(small); cache.slot_images.erase(adjacent);
        puts("PASS: first-page exact image lookup preserves multi-page overlap, equal-base size selection, GPU eligibility and unregister behavior");
    }
};
struct PageManager::Impl {};
PageManager::PageManager(Vulkan::Rasterizer*) {}
PageManager::~PageManager() = default;
void PageManager::ReportFaultSites() {}
void PageManager::NoteImageFault(VAddr, u64, u32, u32, u32, u32) { std::abort(); }
void PageManager::OnGpuMap(VAddr, size_t) {}
void PageManager::OnGpuUnmap(VAddr, size_t) {}
template <bool track> void PageManager::UpdatePageWatchers(VAddr, u64) const {}
template <bool track, bool read>
void PageManager::UpdatePageWatchersForRegion(VAddr, RegionBits&) const {}
template void PageManager::UpdatePageWatchers<true>(VAddr, u64) const;
template void PageManager::UpdatePageWatchers<false>(VAddr, u64) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr, RegionBits&) const;
}
extern "C" {
int runtime_memory_region(uintptr_t addr, uintptr_t* start, uintptr_t* end, int* mapped) {
    assert(addr >= Guest && addr < Guest + GuestSize);
    *start = Guest; *end = Guest + GuestSize; *mapped = 1; return 1;
}
uint64_t runtime_memory_clamp(uintptr_t addr, uint64_t size) {
    assert(addr >= Guest && addr <= Guest + GuestSize);
    return std::min<u64>(size, Guest + GuestSize - addr);
}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) {}
void runtime_memory_read_backing(uintptr_t addr, void* data, uint64_t size) {
    assert(size <= runtime_memory_clamp(addr, size));
    std::memcpy(data, reinterpret_cast<void*>(addr), size);
}
int runtime_memory_write_backing(uintptr_t addr, const void* data, uint64_t size) {
    assert(size <= runtime_memory_clamp(addr, size));
    std::memcpy(reinterpret_cast<void*>(addr), data, size); return 1;
}
}

struct Caches {
    VideoCore::PageManager pages{nullptr};
    VideoCore::BufferCache buffers;
    VideoCore::TextureCache textures;
    Caches(Vulkan::Instance& instance, Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime)
        : buffers{instance, scheduler, runtime, nullptr, textures, pages},
          textures{instance, scheduler, runtime, nullptr, buffers, pages} {
        pages.OnGpuMap(Guest, GuestSize);
    }
};

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    using namespace VideoCore;
    _putenv_s("BB_GUEST_IN_PLACE", "0");
    _putenv_s("BB_WRITE_TRACKING", "0");
    _putenv_s("BB_PREUPLOAD", "0");
    _putenv_s("BB_READBACKS", "1");
    void* mapping = VirtualAlloc(reinterpret_cast<void*>(Guest), GuestSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(mapping == reinterpret_cast<void*>(Guest));
    std::memset(mapping, 0x11, GuestSize);
    Vulkan::Instance instance{-1, false};
    Vulkan::Scheduler scheduler{instance, false};
    Vulkan::Runtime runtime{instance, scheduler};
    Caches caches{instance, scheduler, runtime};
    scheduler.SetSubmitCallback([&](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers(); caches.buffers.SubmitPendingArenaBinds(info);
    });
    assert(!GuestInPlace());
    BbStats::coarse_second.store(100);
    const auto [cold, cold_offset] = caches.buffers.ObtainBuffer(Guest, Size, false);
    const auto [owned, owned_offset] = caches.buffers.ObtainBuffer(Guest + Size, Size, true);
    runtime.FillBuffer(owned, owned_offset, Size, 0x55667788);
    MemoryPressure pressure;
    constexpr u64 Budget = 4ull << 30;
    pressure.Update(Budget, Budget);
    BbStats::coarse_second.store(110);
    const u64 before = BbStats::buffer_evicted_bytes.load();
    caches.buffers.ReclaimMemory(pressure, Budget, false);
    assert(BbStats::buffer_evicted_bytes.load() == before);
    caches.buffers.ReclaimMemory(pressure, Budget, true);
    assert(BbStats::buffer_evicted_bytes.load() == before);
    puts("PASS: unavailable budgets and unfinished GPU use prevent pressure eviction");
    Buffer readback{instance, 0, Size, MemoryType::HostCached, "Budget readback"};
    // Dereference production sparse buffer device addresses in a real compute
    // shader. Transfer copies alone cannot exercise BDA allocation requirements.
    const auto device = instance.GetDevice();
    // Resolve only the test's entry points. Do not emit Vulkan-Hpp init()
    // definitions that differ from vk_platform.cpp in a static link.
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    const auto get_instance_proc = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance_proc(static_cast<VkInstance>(instance.GetInstance()), "vkGetDeviceProcAddr"));
#define LOAD_TEST_ENTRY(name) dispatch.name = reinterpret_cast<PFN_##name>(get_device_proc(static_cast<VkDevice>(device), #name))
    LOAD_TEST_ENTRY(vkCreateDescriptorSetLayout); LOAD_TEST_ENTRY(vkDestroyDescriptorSetLayout);
    LOAD_TEST_ENTRY(vkCreatePipelineLayout); LOAD_TEST_ENTRY(vkDestroyPipelineLayout);
    LOAD_TEST_ENTRY(vkCreateShaderModule); LOAD_TEST_ENTRY(vkDestroyShaderModule);
    LOAD_TEST_ENTRY(vkCreateComputePipelines); LOAD_TEST_ENTRY(vkCreateGraphicsPipelines);
    LOAD_TEST_ENTRY(vkDestroyPipeline); LOAD_TEST_ENTRY(vkCmdBindPipeline);
    LOAD_TEST_ENTRY(vkCmdPushDescriptorSetKHR); LOAD_TEST_ENTRY(vkCmdPushConstants);
    LOAD_TEST_ENTRY(vkCmdDispatch); LOAD_TEST_ENTRY(vkCmdPipelineBarrier2);
    LOAD_TEST_ENTRY(vkCmdBindIndexBuffer); LOAD_TEST_ENTRY(vkCmdBindIndexBuffer2KHR);
    LOAD_TEST_ENTRY(vkCmdBeginRendering); LOAD_TEST_ENTRY(vkCmdEndRendering);
    LOAD_TEST_ENTRY(vkCmdDrawIndexed);
#undef LOAD_TEST_ENTRY
    const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eStorageBuffer,
        1, vk::ShaderStageFlagBits::eCompute};
    auto descriptors = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1, .pBindings = &binding}, nullptr, dispatch).value;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, 16};
    auto layout = device.createPipelineLayoutUnique({.setLayoutCount = 1,
        .pSetLayouts = &*descriptors, .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push}, nullptr, dispatch).value;
    auto module = device.createShaderModuleUnique({.codeSize = sizeof(MEMORY_SAFETY_COMP),
        .pCode = MEMORY_SAFETY_COMP}, nullptr, dispatch).value;
    auto pipeline = device.createComputePipelineUnique({}, {
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
        .layout = *layout}, nullptr, dispatch).value;
    auto check_bda = [&](const Buffer* buffer, u64 offset, u32 expected, u32 bytes = Size) {
        assert(buffer->BufferDeviceAddress() != 0);
        runtime.AccessBuffer(buffer, offset, bytes, vk::PipelineStageFlagBits2::eComputeShader,
                             vk::AccessFlagBits2::eShaderRead);
        runtime.AccessBuffer(&readback, 0, bytes, vk::PipelineStageFlagBits2::eComputeShader,
                             vk::AccessFlagBits2::eShaderWrite);
        runtime.FlushBarriers();
        struct Params { u64 address; u32 count, padding; };
        const Params params{buffer->BufferDeviceAddress() + offset, bytes / 4, 0};
        scheduler.Record([&, params, bytes](vk::CommandBuffer cmd) {
            const vk::DescriptorBufferInfo output{readback.Handle(), 0, bytes};
            const vk::WriteDescriptorSet write{.dstBinding = 0, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &output};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline, dispatch);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, write, dispatch);
            cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params, dispatch);
            cmd.dispatch((params.count + 63) / 64, 1, 1, dispatch);
            const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host}, dispatch);
        });
        scheduler.Finish();
        readback.Invalidate(0, bytes);
        for (u32 n = 0; n < bytes; n += 4) {
            u32 value; std::memcpy(&value, readback.mapped_data.data() + n, 4);
            assert(value == expected);
        }
    };
    const vk::DescriptorSetLayoutBinding vertex_binding{0, vk::DescriptorType::eStorageBuffer,
        1, vk::ShaderStageFlagBits::eVertex};
    auto vertex_descriptors = device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = 1, .pBindings = &vertex_binding}, nullptr, dispatch).value;
    auto vertex_layout = device.createPipelineLayoutUnique({.setLayoutCount = 1,
        .pSetLayouts = &*vertex_descriptors}, nullptr, dispatch).value;
    auto vertex_module = device.createShaderModuleUnique({.codeSize = sizeof(NULL_INDEX_VERT),
        .pCode = NULL_INDEX_VERT}, nullptr, dispatch).value;
    const vk::PipelineShaderStageCreateInfo stage{.stage = vk::ShaderStageFlagBits::eVertex,
        .module = *vertex_module, .pName = "main"};
    const vk::PipelineVertexInputStateCreateInfo input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology = vk::PrimitiveTopology::ePointList};
    const vk::Viewport viewport{0, 0, 1, 1, 0, 1};
    const vk::Rect2D scissor{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo view{.viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor};
    const vk::PipelineRasterizationStateCreateInfo raster{.rasterizerDiscardEnable = true,
        .polygonMode = vk::PolygonMode::eFill, .lineWidth = 1};
    const vk::PipelineMultisampleStateCreateInfo msaa{.rasterizationSamples = vk::SampleCountFlagBits::e1};
    const vk::PipelineColorBlendStateCreateInfo blend{};
    const vk::PipelineRenderingCreateInfo rendering{};
    auto vertex_pipeline = device.createGraphicsPipelineUnique({}, {.pNext = &rendering,
        .stageCount = 1, .pStages = &stage, .pVertexInputState = &input,
        .pInputAssemblyState = &assembly, .pViewportState = &view,
        .pRasterizationState = &raster, .pMultisampleState = &msaa,
        .pColorBlendState = &blend, .layout = *vertex_layout}, nullptr, dispatch).value;
    auto check_indices = [&](const Buffer* buffer, vk::IndexType type, u32 first_index,
                             u32 bytes, bool bounded, u32 expected) {
        runtime.FillBuffer(&readback, 0, 28, 0xffffffffu);
        runtime.AccessBuffer(buffer, 0, bytes, vk::PipelineStageFlagBits2::eIndexInput,
                             vk::AccessFlagBits2::eIndexRead);
        runtime.AccessBuffer(&readback, 0, 28, vk::PipelineStageFlagBits2::eVertexShader,
                             vk::AccessFlagBits2::eShaderWrite);
        runtime.FlushBarriers();
        scheduler.Record([&, buffer, type, first_index, bytes, bounded](vk::CommandBuffer cmd) {
            const vk::DescriptorBufferInfo output{readback.Handle(), 0, 28};
            const vk::WriteDescriptorSet write{.dstBinding = 0, .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &output};
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *vertex_pipeline, dispatch);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *vertex_layout, 0, write, dispatch);
            if (bounded) cmd.bindIndexBuffer2KHR(buffer->Handle(), 0, bytes, type, dispatch);
            else cmd.bindIndexBuffer(buffer->Handle(), 0, type, dispatch);
            cmd.beginRendering({.renderArea = {{0, 0}, {1, 1}}, .layerCount = 1}, dispatch);
            cmd.drawIndexed(1, 7, first_index, 0, 0, dispatch);
            cmd.endRendering(dispatch);
            const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eVertexShader,
                .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host}, dispatch);
        });
        scheduler.Finish(); readback.Invalidate(0, 28);
        for (u32 n = 0; n < 7; ++n) {
            u32 value; std::memcpy(&value, readback.mapped_data.data() + n * 4, 4);
            assert(value == expected);
        }
    };
    Buffer sample_indices{instance, 0, 16, MemoryType::DeviceLocal, "Partial index fixture"};
    runtime.FillBuffer(&sample_indices, 0, 16, 0x00070007);
    for (auto type : {vk::IndexType::eUint16, vk::IndexType::eUint32}) {
        for (bool bounded : {false, true}) {
            if (bounded && !instance.IsMaintenance5Supported()) continue;
            check_indices(caches.buffers.GetNullIndexBuffer(), type, 0, 16, bounded, 0);
            check_indices(caches.buffers.GetNullIndexBuffer(), type, 100, 16, bounded, 0);
            check_indices(&sample_indices, type, 0, 8, bounded,
                          type == vk::IndexType::eUint16 ? 7 : 0x00070007);
            check_indices(&sample_indices, type, type == vk::IndexType::eUint16 ? 4 : 2,
                          8, bounded, bounded ? 0 : type == vk::IndexType::eUint16 ? 7 : 0x00070007);
        }
    }
    puts("PASS: real indexed draws read zero from null storage and beyond its bounds, 16/32-bit; mapped-size binding is bounded");
    auto check = [&](const Buffer* buffer, u64 offset, u32 expected) {
        const vk::BufferCopy region{offset, 0, Size};
        runtime.CopyBuffer(buffer, &readback, std::span{&region, 1});
        scheduler.Finish();
        readback.Invalidate(0, Size);
        for (u32 n = 0; n < Size; n += 4) {
            u32 actual; std::memcpy(&actual, readback.mapped_data.data() + n, 4);
            if (actual != expected) {
                std::fprintf(stderr, "Buffer mismatch: byte %u, actual %08x, expected %08x\n", n, actual, expected);
                std::abort();
            }
        }
    };
    check(cold, cold_offset, 0x11111111);
    check_bda(cold, cold_offset, 0x11111111);
    check_bda(caches.buffers.GetNullIndexBuffer(), 0, 0, 16);
    puts("PASS: sparse BDA shader reads match guest bytes; immutable null index storage is zero");
    check(owned, owned_offset, 0x55667788);
    caches.buffers.ReclaimMemory(pressure, Budget, true);
    assert(BbStats::buffer_evicted_bytes.load() > before);
    assert(BbStats::pending_buffer_bytes.load() != 0);
    // Rebind immediately, before waiting for the retired slots. Its bind must be
    // in a different sparse batch; the old slot must remain unavailable.
    std::memset(mapping, 0x22, Size);
    const auto [rebound, rebound_offset] = caches.buffers.ObtainBuffer(Guest, Size, false);
    check(rebound, rebound_offset, 0x22222222);
    check(owned, owned_offset, 0x55667788);
    caches.buffers.ReclaimMemory(pressure, Budget, true);
    assert(BbStats::pending_buffer_bytes.load() == 0);
    puts("PASS: legacy buffer eviction waits for GPU retirement, rebind reads fresh CPU bytes; GPU data preserved");

    TextureCache::ImageDesc desc;
    desc.info.guest_address = Guest + (2ull << 20);
    desc.info.guest_size = Size;
    desc.info.type = AmdGpu::ImageType::Color2D;
    desc.info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    desc.info.size = {128, 128, 1}; desc.info.pitch = 128;
    desc.info.num_bits = 32; desc.info.resources = {1, 1};
    desc.info.mips_layout[0] = {Size, 128, 128, 0};
    const auto id = caches.textures.FindImage(desc);
    vk::ClearValue clear{};
    clear.color.float32 = std::array{0.f, 1.f, 0.f, 1.f};
    runtime.ClearImage(&caches.textures.GetImage(id), {}, clear);
    const auto [alias, alias_offset] = caches.buffers.ObtainBuffer(desc.info.guest_address, Size, false, true);
    check(alias, alias_offset, 0xff00ff00);
    assert(caches.buffers.IsRegionGpuModified(desc.info.guest_address, Size));
    const auto [small, small_offset] = caches.buffers.ObtainBuffer(desc.info.guest_address, 4096, false);
    assert(small == alias && small_offset == alias_offset); // Must not stream stale guest RAM.
    BbStats::coarse_second.store(120);
    caches.buffers.ReclaimMemory(pressure, Budget, true);
    check(alias, alias_offset, 0xff00ff00);
    puts("PASS: image-to-buffer writes on Windows stay GPU-owned under memory pressure");

    auto depth = desc;
    depth.info.guest_address += Size;
    depth.info.pixel_format = vk::Format::eD32Sfloat;
    depth.info.props.is_depth = true;
    depth.info.meta_info.htile_addr = Guest + (3ull << 20);
    depth.view_info.format = vk::Format::eD32Sfloat;
    depth.type = TextureCache::BindingType::DepthTarget;
    const auto depth_id = caches.textures.FindImage(depth);
    (void)caches.textures.FindDepthTarget(depth_id, depth);
    assert(caches.textures.IsMeta(depth.info.meta_info.htile_addr) == TextureCache::MetaType::HTile);
    const auto [meta, meta_offset] = caches.buffers.ObtainBuffer(depth.info.meta_info.htile_addr, Size, false, true);
    check(meta, meta_offset, 0xf);
    assert(caches.buffers.IsRegionGpuModified(depth.info.meta_info.htile_addr, Size));
    BbStats::coarse_second.store(130);
    caches.buffers.ReclaimMemory(pressure, Budget, true);
    check(meta, meta_offset, 0xf);
    puts("PASS: GPU-filled HTile metadata retains ownership under memory pressure");

    // Contents of a fully unmapped range are dead, including previously GPU-owned ones.
    // A previously recorded read must still see its bytes before the unbind executes.
    const vk::BufferCopy last_read{meta_offset, 0, Size};
    runtime.CopyBuffer(meta, &readback, std::span{&last_read, 1});
    const auto allocated = BbStats::residency_alloc_bytes.load();
    caches.buffers.UnmapInPlace(Guest, GuestSize);
    auto freed = caches.buffers.ReclaimMemory(pressure, Budget, true);
    scheduler.Finish();
    readback.Invalidate(0, Size);
    for (u32 n = 0; n < Size; n += 4) {
        u32 value; std::memcpy(&value, readback.mapped_data.data() + n, 4);
        assert(value == 0xf);
    }
    freed += caches.buffers.ReclaimMemory(pressure, Budget, true);
    assert(freed > 0 && BbStats::residency_alloc_bytes.load() < allocated);
    puts("PASS: recorded reads survive guest unmaps; empty chunks return after unbind completion");
    const auto after_free = BbStats::residency_alloc_bytes.load();
    const auto [tight, tight_offset] = caches.buffers.ObtainBuffer(Guest, Size, false);
    assert(BbStats::residency_alloc_bytes.load() - after_free == Size);
    check(tight, tight_offset, 0x22222222);
    check_bda(tight, tight_offset, 0x22222222);
    puts("PASS: critical pressure allocates the requested range without a speculative 64 MiB pool");
    MemoryPressure normal;
    normal.Update(1ull << 30, Budget);
    caches.buffers.ReclaimMemory(normal, 1ull << 30, true);
    constexpr u32 Large = 40u << 20;
    const auto [first, first_offset] = caches.buffers.ObtainBuffer(Guest + (16ull << 20), Large, false);
    check_bda(first, first_offset, 0x11111111);
    const auto [spare, spare_offset] = caches.buffers.ObtainBuffer(Guest + (64ull << 20), Large, false);
    check_bda(spare, spare_offset, 0x11111111);
    check_bda(spare, spare_offset + Large - Size, 0x11111111);
    assert(BbStats::residency_alloc_bytes.load() >= (128ull << 20));
    puts("PASS: shader reads survive demand, asynchronous spare and tight sparse backing allocations");
    TextureLookupTest::Run(caches.textures);
    Vulkan::StagingBufferPool pool{instance,scheduler};
    const auto big=pool.Request(32ull<<20,MemoryType::HostUncached);
    const auto held=pool.Request(20ull<<20,MemoryType::HostUncached,0,true);
    assert(held.buffer!=big.buffer); // Current-tick and held storage must not alias.
    std::memset(big.mapped,0x44,big.size); big.Flush();
    const vk::BufferCopy capture{big.size-Size,0,Size};
    runtime.CopyBuffer(big.buffer,&readback,std::span{&capture,1});
    scheduler.Finish(); readback.Invalidate(0,Size);
    for (u32 n=0;n<Size;++n) assert(readback.mapped_data[n]==0x44);
    const auto reused=pool.Request(20ull<<20,MemoryType::HostUncached);
    assert(reused.buffer==big.buffer && reused.size==(20ull<<20));
    std::memset(reused.mapped,0x22,reused.size); reused.Flush();
    const vk::BufferCopy fresh{reused.size-Size,0,Size};
    runtime.CopyBuffer(reused.buffer,&readback,std::span{&fresh,1});
    scheduler.Finish(); readback.Invalidate(0,Size);
    for (u32 n=0;n<Size;++n) assert(readback.mapped_data[n]==0x22);
    const auto larger=pool.Request(48ull<<20,MemoryType::HostUncached);
    assert(larger.buffer!=big.buffer && larger.buffer!=held.buffer);
    pool.FreeDeferred(held);
    scheduler.Finish();
    puts("PASS: retired 32 MiB staging serves 20 MiB uploads, GPU reads stay correct, held/in-flight/undersized storage is excluded");
}
