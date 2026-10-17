// SPDX-License-Identifier: GPL-2.0-or-later
// Compile the production implementation here to inject a fake NGX boundary without
// introducing a driver override or alternate lifecycle into the shipped executable.
// Vulkan-Hpp adds fields to DispatchLoaderBase in debug headers. The implementation
// reads the production global dispatcher, so its headers must retain production layout.
#ifndef NDEBUG
#define NDEBUG
#endif
#include "../engine/renderer/shadps4/video_core/renderer_vulkan/vk_dlss.cpp"
#include "video_core/buffer_cache/buffer.h"
#undef NDEBUG
#include <cassert>
#include <crtdbg.h>
#include <map>
#include <set>
#include <vk_mem_alloc.h>

namespace Vulkan {
struct DlssLifetimeTest {
    struct Parameters {
        void **table;
        std::map<std::string, int> integers;
    };
    static inline std::set<Parameters *> parameters;
    static inline std::map<NgxHandle *, u64> handles;
    static inline Scheduler *scheduler;
    static inline unsigned creates, evaluates, releases, shutdowns, serial;
    static inline bool fail_create, partial, fail_allocate;
    static inline int reset;
    static inline std::vector<std::string> events;
    static Parameters &Param(NgxParameter *p) { return *reinterpret_cast<Parameters *>(p); }
    static void SetVoid(NgxParameter *, const char *, void *) {}
    static void SetInt(NgxParameter *p, const char *name, int value) {
        Param(p).integers[name] = value;
    }
    static void SetUint(NgxParameter *p, const char *name, unsigned value) {
        SetInt(p, name, int(value));
    }
    static void SetFloat(NgxParameter *, const char *, float) {}
    static NgxResult Allocate(NgxParameter **out) {
        static void *table[16]{};
        table[ParamSlot::SetVoid] = reinterpret_cast<void *>(&SetVoid);
        table[ParamSlot::SetInt] = reinterpret_cast<void *>(&SetInt);
        table[ParamSlot::SetUint] = reinterpret_cast<void *>(&SetUint);
        table[ParamSlot::SetFloat] = reinterpret_cast<void *>(&SetFloat);
        *out = nullptr;
        if (fail_allocate)
            return 0xBAD00001;
        auto *p = new Parameters{table, {}};
        parameters.insert(p);
        *out = reinterpret_cast<NgxParameter *>(p);
        return 1;
    }
    static NgxResult Destroy(NgxParameter *p) {
        auto *value = reinterpret_cast<Parameters *>(p);
        assert(parameters.erase(value) == 1);
        events.push_back("parameters");
        delete value;
        return 1;
    }
    static NgxResult Create(VkDevice device, VkCommandBuffer command, int feature, NgxParameter *p,
                            NgxHandle **out) {
        assert(device && command && feature == kFeatureSuperSampling);
        assert(Param(p).integers.at("Width") > 0 && !Param(p).integers.contains("stale"));
        ++creates;
        *out = nullptr;
        if (!fail_create || partial) {
            *out = reinterpret_cast<NgxHandle *>(uintptr_t(++serial) * 16);
            handles[*out] = scheduler->CurrentTick();
        }
        return fail_create ? 0xBAD00001 : 1;
    }
    static NgxResult Evaluate(VkCommandBuffer command, const NgxHandle *handle,
                              const NgxParameter *params, void *) {
        assert(command && handles.contains(const_cast<NgxHandle *>(handle)));
        handles[const_cast<NgxHandle *>(handle)] = scheduler->CurrentTick();
        reset = Param(const_cast<NgxParameter *>(params)).integers.at("Reset");
        ++evaluates;
        return 1;
    }
    static NgxResult Release(NgxHandle *handle) {
        assert(handles.contains(handle) && scheduler->IsFree(handles.at(handle)));
        handles.erase(handle);
        ++releases;
        events.push_back("feature");
        return 1;
    }
    static NgxResult Shutdown() {
        assert(handles.empty() && parameters.empty());
        ++shutdowns;
        events.push_back("shutdown");
        return 1;
    }
    static void Run(const Instance &instance, Scheduler &queue) {
        scheduler = &queue;
        {
            DlssUpscaler::Impl impl{instance, queue};
            impl.available = impl.initialized = true;
            impl.allocate_parameters = Allocate;
            impl.destroy_parameters = Destroy;
            impl.create_feature = Create;
            impl.evaluate_feature = Evaluate;
            impl.release_feature = Release;
            impl.shutdown = Shutdown;
            assert(Allocate(&impl.capabilities) == 1);
            Fsr4Upscaler::Frame f{};
            f.render_width = 128;
            f.render_height = 72;
            f.output.width = 192;
            f.output.height = 108;
            f.preset = BbSettings::Quality;
            f.cmdbuf = queue.CommandBuffer();
            const auto tick = queue.CurrentTick();
            assert(impl.Record(f) && reset == 1 && creates == 1);
            Param(impl.params).integers["stale"] = 1;
            assert(impl.Record(f) && reset == 0 && creates == 1);
            assert(!impl.OnFrameStart(false) && queue.CurrentTick() == tick);

            f.render_width = 96;
            assert(!impl.Record(f) && impl.retire_pending && releases == 0);
            assert(queue.CurrentTick() == tick && queue.CommandBuffer() == f.cmdbuf);
            assert(impl.OnFrameStart(false) && releases == 1);
            f.cmdbuf = queue.CommandBuffer();
            assert(impl.Record(f) && reset == 1 && creates == 2);

            queue.Flush();
            f.cmdbuf = queue.CommandBuffer();
            f.render_width = 128;
            const auto submitted_tick = queue.CurrentTick();
            assert(impl.Record(f) && reset == 1 && creates == 3 && releases == 2);
            assert(queue.CurrentTick() == submitted_tick && queue.CommandBuffer() == f.cmdbuf);

            assert(impl.OnFrameStart(true));
            f.cmdbuf = queue.CommandBuffer();
            fail_create = partial = true;
            assert(!impl.Record(f) && impl.feature && impl.retire_pending);
            const auto attempts = creates, calls = evaluates;
            // The key matches the previous successful key: a partial failed handle
            // still must not be evaluated by another draw in the same submission.
            assert(!impl.Record(f) && creates == attempts && evaluates == calls);
            assert(impl.OnFrameStart(false) && !impl.feature);
            fail_create = partial = false;
            f.cmdbuf = queue.CommandBuffer();
            assert(impl.Record(f) && reset == 1);

            assert(impl.OnFrameStart(true));
            f.cmdbuf = queue.CommandBuffer();
            fail_allocate = true;
            assert(!impl.Record(f) && !impl.params && !impl.feature);
            fail_allocate = false;
            assert(impl.Record(f) && reset == 1);
        }
        assert(handles.empty() && parameters.empty() && shutdowns == 1);
        assert(events.back() == "shutdown");
        std::puts("PASS: NGX current-submission deferral, submitted-work wait without closing "
                  "the pass, history reset, fresh parameters, partial handle guard, "
                  "allocation retry and destruction order");
    }
};
} // namespace Vulkan

struct TestImages {
    const Vulkan::Instance &instance;
    vk::detail::DispatchLoaderDynamic &dispatch;
    std::vector<VkImage> images;
    std::vector<VmaAllocation> allocations;
    std::vector<VkImageView> views;
    Vulkan::Fsr4Upscaler::Image Add(vk::Format format, u32 w, u32 h, bool depth = false) {
        const VkImageCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VkFormat(format),
            .extent = {w, h, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     (depth ? 0u : VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage image;
        VmaAllocation allocation;
        const VmaAllocationCreateInfo memory{.usage = VMA_MEMORY_USAGE_AUTO};
        assert(vmaCreateImage(instance.GetAllocator(), &info, &memory, &image, &allocation,
                              nullptr) == VK_SUCCESS);
        const VkImageViewCreateInfo view_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VkFormat(format),
            .subresourceRange = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                 1, 0, 1}};
        VkImageView view;
        assert(dispatch.vkCreateImageView(VkDevice(instance.GetDevice()), &view_info, nullptr,
                                          &view) == VK_SUCCESS);
        images.push_back(image);
        allocations.push_back(allocation);
        views.push_back(view);
        return {image, view, w, h, format};
    }
    ~TestImages() {
        for (size_t i = 0; i < images.size(); ++i) {
            dispatch.vkDestroyImageView(VkDevice(instance.GetDevice()), views[i], nullptr);
            vmaDestroyImage(instance.GetAllocator(), images[i], allocations[i]);
        }
    }
};

static void RealNgx(const Vulkan::Instance &instance, Vulkan::Scheduler &queue) {
    static vk::detail::DynamicLoader loader;
    const auto gipa = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        gipa(VkInstance(instance.GetInstance()), "vkGetDeviceProcAddr"));
    vk::detail::DispatchLoaderDynamic d;
#define LOAD(name)                                                                                 \
    d.name = reinterpret_cast<PFN_##name>(gdpa(VkDevice(instance.GetDevice()), #name));            \
    assert(d.name)
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkCmdPipelineBarrier2);
    LOAD(vkCmdClearColorImage);
    LOAD(vkCmdClearDepthStencilImage);
    LOAD(vkCmdCopyImageToBuffer);
#undef LOAD
    Vulkan::DlssUpscaler dlss(instance, queue);
    if (!dlss.Available())
        std::printf("DLSS test unavailable: %s\n", dlss.Problem());
    assert(dlss.Available());
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        dlss.OnFrameStart(true);
        const bool native = cycle % 4 == 3;
        const u32 ow = cycle < 4 ? 192 : 256, oh = cycle < 4 ? 108 : 144;
        const u32 w = native ? ow : ow * 2 / 3, h = native ? oh : oh * 2 / 3;
        TestImages images{instance, d};
        Vulkan::Fsr4Upscaler::Frame f{};
        f.color = images.Add(vk::Format::eR16G16B16A16Sfloat, w, h);
        f.depth = images.Add(vk::Format::eD32Sfloat, w, h, true);
        f.motion = images.Add(vk::Format::eR16G16Sfloat, w, h);
        f.output = images.Add(vk::Format::eR16G16B16A16Sfloat, ow, oh);
        f.render_width = w;
        f.render_height = h;
        f.preset = native ? BbSettings::NativeAA : int(BbSettings::Quality) + int(cycle % 3);
        f.frame_ms = 1000.0f / 60;
        f.near_plane = 0.1f;
        f.far_plane = 3000;
        f.vertical_fov = 1;
        const auto cmd = queue.CommandBuffer();
        for (const auto image : {f.color, f.depth, f.motion, f.output}) {
            const bool depth = image.format == vk::Format::eD32Sfloat;
            const VkImageSubresourceRange range{
                depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier2 barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                          .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                          .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                          .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .image = VkImage(image.image),
                                          .subresourceRange = range};
            VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                        .imageMemoryBarrierCount = 1,
                                        .pImageMemoryBarriers = &barrier};
            d.vkCmdPipelineBarrier2(VkCommandBuffer(cmd), &dependency);
            if (depth) {
                const VkClearDepthStencilValue value{1, 0};
                d.vkCmdClearDepthStencilImage(VkCommandBuffer(cmd), VkImage(image.image),
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                              &range);
            } else {
                VkClearColorValue value{};
                if (image.image == f.color.image)
                    for (float &channel : value.float32)
                        channel = 0.25f;
                d.vkCmdClearColorImage(VkCommandBuffer(cmd), VkImage(image.image),
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
            }
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            d.vkCmdPipelineBarrier2(VkCommandBuffer(cmd), &dependency);
        }
        f.cmdbuf = cmd;
        assert(dlss.Record(f));
        queue.Finish();
        assert(!dlss.OnFrameStart(false));
        f.cmdbuf = queue.CommandBuffer();
        f.reset = false;
        assert(dlss.Record(f));
        VideoCore::Buffer readback{instance, 0, u64(ow) * oh * 8, VideoCore::MemoryType::HostCached,
                                   "NGX fixture readback"};
        VkImageMemoryBarrier2 image_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = VkImage(f.output.image),
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                    .imageMemoryBarrierCount = 1,
                                    .pImageMemoryBarriers = &image_barrier};
        d.vkCmdPipelineBarrier2(VkCommandBuffer(f.cmdbuf), &dependency);
        const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageExtent = {ow, oh, 1}};
        d.vkCmdCopyImageToBuffer(VkCommandBuffer(f.cmdbuf), VkImage(f.output.image),
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VkBuffer(readback.Handle()),
                                 1, &copy);
        const VkMemoryBarrier2 memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                      .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        dependency.imageMemoryBarrierCount = 0;
        dependency.pImageMemoryBarriers = nullptr;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &memory;
        d.vkCmdPipelineBarrier2(VkCommandBuffer(f.cmdbuf), &dependency);
        queue.Finish();
        readback.Invalidate(0, u64(ow) * oh * 8);
        const auto *pixels = reinterpret_cast<const uint16_t *>(readback.mapped_data.data());
        bool nonzero = false;
        for (size_t i = 0; i < size_t(ow) * oh * 4; ++i) {
            assert((pixels[i] & 0x7c00) != 0x7c00); // All half-float outputs are finite.
            if (i % 4 != 3 && (pixels[i] & 0x7fff))
                nonzero = true;
        }
        assert(nonzero);
        dlss.OnFrameStart(true); // Retire before the input/output views are destroyed.
    }
    std::puts("PASS: real NGX initialize, 8 quality/DLAA/size recreations, 16 evaluations, "
              "finite nonzero GPU output, provider retirement and shutdown");
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _putenv_s("BB_PIPELINE_CACHE", "0");
    _putenv_s("BB_NULL_GPU", "1");
    _putenv_s("BB_DLSS_DIR", std::filesystem::absolute("out").string().c_str());
    Vulkan::Instance instance{-1, false};
    Vulkan::Scheduler queue{instance, false};
    Vulkan::DlssLifetimeTest::Run(instance, queue);
    if (argc == 2 && !std::strcmp(argv[1], "--real"))
        RealNgx(instance, queue);
}
