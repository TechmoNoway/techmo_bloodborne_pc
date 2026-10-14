// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <crtdbg.h>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "pipeline_prep_vert.h"
#include "pipeline_prep_frag.h"

namespace Vulkan {
struct PipelinePrecompileTest {
    static auto Consume(PipelineCache& cache, PipelineSelection& selection) {
        assert(cache.precompiler);
        cache.QueuePreparedPipeline(selection);
        const_cast<u32*>(selection.infos[u32(Shader::SwStage::Vertex)]->user_data.data())[0]=999;
        auto ready = cache.precompiler->TakeOrReserve(selection.graphics_key);
        assert(ready && ready->pipeline);
        assert(ready->snapshot->infos[u32(Shader::SwStage::Vertex)]->user_data[0] == 123);
        ready->pipeline->BindRuntimeStages(selection.infos);
        ready->snapshot.reset(); // Runtime stage pointers must not retain the freed snapshot.
        assert(&ready->pipeline->GetStage(Shader::SwStage::Vertex) == selection.infos[u32(Shader::SwStage::Vertex)]);
        return std::move(ready->pipeline);
    }
    static auto Inline(PipelineCache& cache, PipelineSelection& selection) {
        GraphicsPipeline::SerializationSupport data;
        return std::make_unique<GraphicsPipeline>(cache.instance, cache.scheduler,
            cache.desc_heap, cache.profile, selection.graphics_key, *cache.pipeline_cache,
            selection.infos, selection.runtime_infos, selection.fetch_shader,
            selection.modules, data, false);
    }
};
}

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _putenv_s("BB_PIPELINE_CACHE", "0"); // Do not read or modify the player's shader cache.
    _putenv_s("BB_PIPELINE_PREP", "1");
    _putenv_s("BB_NULL_GPU", "1");
    using namespace Vulkan;
    Instance instance{-1, false};
    if (!instance.IsVertexInputDynamicState()) {
        puts("SKIP: background pipeline preparation requires dynamic vertex input"); return 0;
    }
    Scheduler scheduler{instance, false};
    AmdGpu::Liverpool gpu;
    const auto device = instance.GetDevice();
    // Explicit entry points avoid conflicting Vulkan-Hpp inline init definitions
    // between asserted fixture headers and production platform headers.
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    const auto get_instance = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance(static_cast<VkInstance>(instance.GetInstance()), "vkGetDeviceProcAddr"));
#define LOAD(name) dispatch.name = reinterpret_cast<PFN_##name>(get_device(static_cast<VkDevice>(device), #name))
    LOAD(vkCreateShaderModule); LOAD(vkDestroyShaderModule);
    LOAD(vkCreateImageView); LOAD(vkDestroyImageView);
    LOAD(vkCmdBindPipeline); LOAD(vkCmdBeginRendering); LOAD(vkCmdEndRendering);
    LOAD(vkCmdDraw); LOAD(vkCmdSetVertexInputEXT);
    LOAD(vkCmdPipelineBarrier2); LOAD(vkCmdCopyImageToBuffer);
#undef LOAD
    auto vertex = device.createShaderModuleUnique({.codeSize = sizeof(PIPELINE_PREP_VERT),
        .pCode = PIPELINE_PREP_VERT}, nullptr, dispatch).value;
    auto fragment = device.createShaderModuleUnique({.codeSize = sizeof(PIPELINE_PREP_FRAG),
        .pCode = PIPELINE_PREP_FRAG}, nullptr, dispatch).value;
    PipelineCache cache{instance, scheduler, &gpu, 16};
    Shader::Info vs{}, fs{};
    vs.hw_stage = Shader::HwStage::Vertex; vs.sw_stage = Shader::SwStage::Vertex; vs.pgm_hash = 1;
    fs.hw_stage = Shader::HwStage::Fragment; fs.sw_stage = Shader::SwStage::Fragment; fs.pgm_hash = 2;
    std::array<u32, 1> ud{123}; vs.user_data = ud; vs.flattened_ud_buf = {123};
    PipelineSelection selection{};
    selection.infos[u32(Shader::SwStage::Vertex)] = &vs;
    selection.infos[u32(Shader::SwStage::Fragment)] = &fs;
    selection.modules[u32(Shader::SwStage::Vertex)] = *vertex;
    selection.modules[u32(Shader::SwStage::Fragment)] = *fragment;
    auto& key = selection.graphics_key;
    key.prim_type = AmdGpu::PrimitiveType::TriangleList;
    key.logic_op = AmdGpu::ColorControl::LogicOp::Copy;
    key.polygon_mode = AmdGpu::PolygonMode::Fill;
    key.num_samples = key.depth_samples = key.color_samples[0] = 1;
    key.num_color_attachments = 1;
    key.stage_hashes[u32(Shader::SwStage::Vertex)] = 1;
    key.stage_hashes[u32(Shader::SwStage::Fragment)] = 2;
    key.color_buffers[0].data_format = AmdGpu::DataFormat::Format8_8_8_8;
    key.color_buffers[0].num_format = AmdGpu::NumberFormat::Unorm;
    key.write_masks[0] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    auto prepared = PipelinePrecompileTest::Consume(cache, selection);
    auto inline_pipeline = PipelinePrecompileTest::Inline(cache, selection);
    VideoCore::UniqueImage target{device, instance.GetAllocator()};
    target.Create({.imageType = vk::ImageType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {16,16,1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1, .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc});
    auto view = device.createImageViewUnique({.image = vk::Image(target),
        .viewType = vk::ImageViewType::e2D, .format = vk::Format::eR8G8B8A8Unorm,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}}, nullptr, dispatch).value;
    VideoCore::Buffer readback{instance, 0, 16*16*4, VideoCore::MemoryType::HostCached, "Pipeline prep readback"};
    bool first = true;
    for (const auto* pipeline : {prepared.get(), inline_pipeline.get()}) {
        scheduler.Record([&, pipeline, first](vk::CommandBuffer cmd) {
            const vk::ImageMemoryBarrier2 begin{.srcStageMask = first ? vk::PipelineStageFlags2{} : vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = first ? vk::AccessFlags2{} : vk::AccessFlagBits2::eTransferRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .oldLayout = first ? vk::ImageLayout::eUndefined : vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal, .image = vk::Image(target),
                .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}};
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&begin}, dispatch);
            DynamicState state{}; state.Invalidate();
            state.viewports.push_back({0,0,16,16,0,1});
            state.scissors.push_back({{0,0},{16,16}});
            state.line_width = 1; state.color_write_masks.fill(key.write_masks[0]);
            state.Commit(instance, cmd);
            cmd.setVertexInputEXT({}, {}, dispatch);
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle(), dispatch);
            const vk::RenderingAttachmentInfo attachment{.imageView=*view,
                .imageLayout=vk::ImageLayout::eColorAttachmentOptimal, .loadOp=vk::AttachmentLoadOp::eClear,
                .storeOp=vk::AttachmentStoreOp::eStore,
                .clearValue={.color={.float32=std::array<float,4>{0.f,0.f,0.f,0.f}}}};
            cmd.beginRendering({.renderArea={{0,0},{16,16}},.layerCount=1,
                .colorAttachmentCount=1,.pColorAttachments=&attachment}, dispatch);
            cmd.draw(3,1,0,0,dispatch); cmd.endRendering(dispatch);
            auto copy_barrier = begin;
            copy_barrier.srcStageMask=vk::PipelineStageFlagBits2::eColorAttachmentOutput;
            copy_barrier.srcAccessMask=vk::AccessFlagBits2::eColorAttachmentWrite;
            copy_barrier.dstStageMask=vk::PipelineStageFlagBits2::eCopy;
            copy_barrier.dstAccessMask=vk::AccessFlagBits2::eTransferRead;
            copy_barrier.oldLayout=vk::ImageLayout::eColorAttachmentOptimal;
            copy_barrier.newLayout=vk::ImageLayout::eTransferSrcOptimal;
            cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&copy_barrier}, dispatch);
            const vk::BufferImageCopy copy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
                .imageExtent={16,16,1}};
            cmd.copyImageToBuffer(vk::Image(target), vk::ImageLayout::eTransferSrcOptimal,
                                 readback.Handle(), copy, dispatch);
            const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host}, dispatch);
        });
        scheduler.Finish(); readback.Invalidate(0,16*16*4);
        for (u32 i=0;i<16*16*4;i+=4) {
            const auto* pixel=readback.mapped_data.data()+i;
            if (pixel[0]!=0 || pixel[1]!=255 || pixel[2]!=0 || pixel[3]!=255)
                std::fprintf(stderr, "%s pixel %u: %u %u %u %u\n", first ? "prepared" : "inline", i/4,
                             pixel[0], pixel[1], pixel[2], pixel[3]);
            assert(pixel[0]==0 && pixel[1]==255 && pixel[2]==0 && pixel[3]==255);
        }
        first=false;
    }
    cache.Sync();
    puts("PASS: background and foreground production graphics pipelines render identical pixels; snapshots released before drawing, driver caches merged after drain");
}
