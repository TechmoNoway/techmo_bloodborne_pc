// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/hash.h"
#include "pipeline_prep_comp.h"
#include "pipeline_prep_frag.h"
#include "pipeline_prep_quad.h"
#include "pipeline_prep_vert.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/preparation_shader_params.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include <array>
#include <cassert>
#include <chrono>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace AmdGpu {
struct LiverpoolTaskTest {
    static void SelectGraphicsQueue(Liverpool& gpu) { gpu.curr_qid = Liverpool::GfxQueueId; }
};
}

namespace Vulkan {
struct PipelinePrecompileTest {
    static auto Consume(PipelineCache &cache, PipelineSelection &selection, bool required = false) {
        assert(cache.precompiler);
        std::optional<PipelineCache::PreparedPipeline> ready;
        if (required) {
            const auto before = cache.precompiler->RequiredQueued();
            ready = cache.precompiler->TakeOrQueue(
                selection.graphics_key, [&] { return cache.MakeGraphicsWork(selection); });
            assert(cache.precompiler->RequiredQueued() == before + 1);
        } else {
            cache.QueuePreparedPipeline(selection);
            const_cast<u32 *>(selection.infos[u32(Shader::SwStage::Vertex)]->user_data.data())[0] =
                999;
            ready = cache.precompiler->TakeOrReserve(selection.graphics_key);
        }
        assert(ready && ready->pipeline);
        assert(ready->snapshot->infos[u32(Shader::SwStage::Vertex)]->user_data[0] == 123);
        ready->pipeline->BindRuntimeStages(selection.infos);
        ready->snapshot.reset(); // Runtime stage pointers must not retain the freed snapshot.
        assert(&ready->pipeline->GetStage(Shader::SwStage::Vertex) ==
               selection.infos[u32(Shader::SwStage::Vertex)]);
        return std::move(ready->pipeline);
    }
    static const Shader::Info *InstallCompute(PipelineCache &cache,
                                             const AmdGpu::ComputeProgram &regs,
                                             vk::ShaderModule module, bool template_ready = true) {
        const auto params = AmdGpu::GetParams(regs);
        auto program =
            std::make_unique<Program>(Shader::HwStage::Compute, Shader::SwStage::Compute, params);
        program->info.srt_info.flattened_bufsize_dw = regs.user_data.size();
        program->info.buffers.push_back(Shader::BufferResource{.is_written = true});
        program->info.RefreshFlatBuf();
        PipelineSelection selection{};
        selection.compute_regs = &regs;
        const auto runtime =
            cache.BuildRuntimeInfo(selection, Shader::HwStage::Compute, Shader::SwStage::Compute);
        Shader::Backend::Bindings binding{};
        program->AddPermut(
            module, Shader::StageSpecialization(program->info, runtime, cache.profile, binding));
        if (template_ready)
            program->info_template = std::make_unique<Shader::Info>(program->info);
        auto *info = &program->info;
        {
            std::unique_lock lock{cache.programs_mutex};
            cache.program_cache.emplace(params.hash, std::move(program));
        }
        cache.ProgramPublished();
        return info;
    }
    static void CheckComputeAdoption(PipelineCache &cache, bool prepared) {
        assert(cache.prepared_compute_hits == (prepared ? 1u : 2u));
        assert(cache.compute_precompiler->RequiredQueued() == (prepared ? 0u : 1u));
    }
    static void CheckPriority(PipelineCache& cache) {
        GraphicsPipelineKey key{};
        key.prim_type = AmdGpu::PrimitiveType::TriangleFan;
        auto result = cache.precompiler->TakeOrQueue(key, [] {
            return [] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (GetThreadPriority(GetCurrentThread()) != THREAD_PRIORITY_NORMAL &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                assert(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL);
                return PipelineCache::PreparedPipeline{};
            };
        });
        assert(result);
        puts("PASS: Windows compiler priority rises to normal while its GPU consumer waits");
    }
    static auto Inline(PipelineCache &cache, PipelineSelection &selection) {
        GraphicsPipeline::SerializationSupport data;
        return std::make_unique<GraphicsPipeline>(
            cache.instance, cache.scheduler, cache.desc_heap, cache.profile, selection.graphics_key,
            *cache.pipeline_cache, selection.infos, selection.runtime_infos, selection.fetch_shader,
            selection.modules, data, false);
    }
};
} // namespace Vulkan

void CheckRegisterReplay() {
    using namespace AmdGpu;
    auto shadow = std::make_unique<Regs>();
    auto reference = std::make_unique<Regs>();
    auto replay = std::make_unique<Regs>();
    u64 checksum = 0, expected = 0;
    const auto header = [](PM4ItOpcode opcode, u32 payload) {
        PM4Type3Header type3{opcode, payload - 1};
        u32 result;
        std::memcpy(&result, &type3, 4);
        return result;
    };
    const std::array<u32, 4> set{header(PM4ItOpcode::SetShReg, 3), 0x200, 9, 7};
    RegDirty dirty;
    auto *packet = reinterpret_cast<const PM4Header *>(set.data());
    Vulkan::DrawPreparation::ApplyPreparationRegisters(*shadow, packet, checksum, &dirty);
    Liverpool::ApplyGraphicsRegisterPacket(*reference, packet, expected);
    assert(checksum == expected && shadow->cs_program.dispatch_initiator == 9 &&
           shadow->cs_program.dim_x == 7);
    RegDelta delta;
    delta.Capture(*shadow, dirty);
    delta.Apply(*replay);
    assert(std::memcmp(&shadow->cs_program, &replay->cs_program, sizeof(ComputeProgram)) == 0);
    const auto saved = shadow->cs_program;
    const std::array<u32, 2> clear{header(PM4ItOpcode::ClearState, 1), 0};
    packet = reinterpret_cast<const PM4Header *>(clear.data());
    Vulkan::DrawPreparation::ApplyPreparationRegisters(*shadow, packet, checksum, &dirty);
    Liverpool::ApplyGraphicsRegisterPacket(*reference, packet, expected);
    assert(checksum == expected && std::memcmp(&shadow->cs_program, &saved, sizeof(saved)) == 0);
    delta.Capture(*shadow, dirty);
    delta.Apply(*replay);
    assert(std::memcmp(&shadow->cs_program, &replay->cs_program, sizeof(saved)) == 0);
    const std::array<u32, 5> dispatch{header(PM4ItOpcode::DispatchDirect, 4), 2, 3, 4, 1};
    packet = reinterpret_cast<const PM4Header *>(dispatch.data());
    Vulkan::DrawPreparation::ApplyPreparationRegisters(*shadow, packet, checksum);
    assert(shadow->cs_program.dim_x == 2 && shadow->cs_program.dim_y == 3 &&
           shadow->cs_program.dim_z == 4 && shadow->cs_program.dispatch_initiator == 1);
    puts("PASS: compute shadow replays partial SH writes, scanner deltas, "
         "CLEAR_STATE and direct dispatch dimensions without changing graphics "
         "checksums");
    alignas(256) std::array<u32, 0x4000> absent_shader{};
    ComputeProgram unknown{};
    assert(!Vulkan::TryPreparationParams(unknown));
    unknown.address = reinterpret_cast<uintptr_t>(absent_shader.data()) >> 8;
    absent_shader[0] = 0xBEEB03FF;
    absent_shader[1] = 0xffffffff;
    assert(!Vulkan::TryPreparationParams(unknown));
    puts("PASS: absent and incomplete speculative shader headers return "
         "unavailable without terminating the game");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _putenv_s("BB_PIPELINE_CACHE",
              "0"); // Do not read or modify the player's shader cache.
    _putenv_s("BB_PIPELINE_PREP", "1");
    _putenv_s("BB_NULL_GPU", "1");
    CheckRegisterReplay();
    using namespace Vulkan;
    Instance instance{-1, false};
    if (!instance.IsVertexInputDynamicState()) {
        puts("SKIP: background pipeline preparation requires dynamic vertex input");
        return 0;
    }
    Scheduler scheduler{instance, false};
    AmdGpu::Liverpool gpu;
    AmdGpu::LiverpoolTaskTest::SelectGraphicsQueue(gpu);
    const auto device = instance.GetDevice();
    // Explicit entry points avoid conflicting Vulkan-Hpp inline init definitions
    // between asserted fixture headers and production platform headers.
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    const auto get_instance =
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance(static_cast<VkInstance>(instance.GetInstance()), "vkGetDeviceProcAddr"));
#define LOAD(name)                                                                                 \
    dispatch.name = reinterpret_cast<PFN_##name>(get_device(static_cast<VkDevice>(device), #name))
    LOAD(vkCreateShaderModule);
    LOAD(vkDestroyShaderModule);
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkCmdBindPipeline);
    LOAD(vkCmdBeginRendering);
    LOAD(vkCmdEndRendering);
    LOAD(vkCmdDraw);
    LOAD(vkCmdSetVertexInputEXT);
    LOAD(vkCmdPipelineBarrier2);
    LOAD(vkCmdCopyImageToBuffer);
    LOAD(vkCmdDispatch);
    LOAD(vkCmdPushDescriptorSetKHR);
#undef LOAD
    auto vertex = device
                      .createShaderModuleUnique(
                          {.codeSize = sizeof(PIPELINE_PREP_VERT), .pCode = PIPELINE_PREP_VERT},
                          nullptr, dispatch)
                      .value;
    auto fragment = device
                        .createShaderModuleUnique(
                            {.codeSize = sizeof(PIPELINE_PREP_FRAG), .pCode = PIPELINE_PREP_FRAG},
                            nullptr, dispatch)
                        .value;
    auto quad = device
                    .createShaderModuleUnique(
                        {.codeSize = sizeof(PIPELINE_PREP_QUAD), .pCode = PIPELINE_PREP_QUAD},
                        nullptr, dispatch)
                    .value;
    auto compute = device
                       .createShaderModuleUnique(
                           {.codeSize = sizeof(PIPELINE_PREP_COMP), .pCode = PIPELINE_PREP_COMP},
                           nullptr, dispatch)
                       .value;
    PipelineCache cache{instance, scheduler, &gpu, 16};
    PipelinePrecompileTest::CheckPriority(cache);
    Shader::Info vs{}, fs{};
    vs.hw_stage = Shader::HwStage::Vertex;
    vs.sw_stage = Shader::SwStage::Vertex;
    vs.pgm_hash = 1;
    fs.hw_stage = Shader::HwStage::Fragment;
    fs.sw_stage = Shader::SwStage::Fragment;
    fs.pgm_hash = 2;
    std::array<u32, 1> ud{123};
    vs.user_data = ud;
    vs.flattened_ud_buf = {123};
    PipelineSelection selection{};
    selection.infos[u32(Shader::SwStage::Vertex)] = &vs;
    selection.infos[u32(Shader::SwStage::Fragment)] = &fs;
    selection.modules[u32(Shader::SwStage::Vertex)] = *vertex;
    selection.modules[u32(Shader::SwStage::Fragment)] = *fragment;
    auto &key = selection.graphics_key;
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
    for (const auto primitive :
         {AmdGpu::PrimitiveType::TriangleList, AmdGpu::PrimitiveType::TriangleStrip,
          AmdGpu::PrimitiveType::RectList, AmdGpu::PrimitiveType::QuadList}) {
        const bool generated = primitive == AmdGpu::PrimitiveType::RectList ||
                               primitive == AmdGpu::PrimitiveType::QuadList;
        if (generated && !instance.IsTessellationSupported())
            continue;
        key.prim_type = primitive;
        selection.modules[u32(Shader::SwStage::Vertex)] = generated ? *quad : *vertex;
        ud[0] = 123;
        auto prepared = PipelinePrecompileTest::Consume(
            cache, selection, primitive == AmdGpu::PrimitiveType::TriangleStrip);
        auto inline_pipeline = PipelinePrecompileTest::Inline(cache, selection);
        VideoCore::UniqueImage target{device, instance.GetAllocator()};
        target.Create({.imageType = vk::ImageType::e2D,
                       .format = vk::Format::eR8G8B8A8Unorm,
                       .extent = {16, 16, 1},
                       .mipLevels = 1,
                       .arrayLayers = 1,
                       .samples = vk::SampleCountFlagBits::e1,
                       .tiling = vk::ImageTiling::eOptimal,
                       .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                vk::ImageUsageFlagBits::eTransferSrc});
        auto view = device
                        .createImageViewUnique(
                            {.image = vk::Image(target),
                             .viewType = vk::ImageViewType::e2D,
                             .format = vk::Format::eR8G8B8A8Unorm,
                             .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}},
                            nullptr, dispatch)
                        .value;
        VideoCore::Buffer readback{instance, 0, 16 * 16 * 4, VideoCore::MemoryType::HostCached,
                                   "Pipeline prep readback"};
        bool first = true;
        for (const auto *pipeline : {prepared.get(), inline_pipeline.get()}) {
            scheduler.Record([&, pipeline, first](vk::CommandBuffer cmd) {
                const vk::ImageMemoryBarrier2 begin{
                    .srcStageMask =
                        first ? vk::PipelineStageFlags2{} : vk::PipelineStageFlagBits2::eCopy,
                    .srcAccessMask =
                        first ? vk::AccessFlags2{} : vk::AccessFlagBits2::eTransferRead,
                    .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                    .oldLayout =
                        first ? vk::ImageLayout::eUndefined : vk::ImageLayout::eTransferSrcOptimal,
                    .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .image = vk::Image(target),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &begin},
                                     dispatch);
                DynamicState state{};
                state.Invalidate();
                state.viewports.push_back({0, 0, 16, 16, 0, 1});
                state.scissors.push_back({{0, 0}, {16, 16}});
                state.line_width = 1;
                state.color_write_masks.fill(key.write_masks[0]);
                state.Commit(instance, cmd);
                cmd.setVertexInputEXT({}, {}, dispatch);
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle(), dispatch);
                const vk::RenderingAttachmentInfo attachment{
                    .imageView = *view,
                    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .loadOp = vk::AttachmentLoadOp::eClear,
                    .storeOp = vk::AttachmentStoreOp::eStore,
                    .clearValue = {.color = {.float32 = std::array<float, 4>{0.f, 0.f, 0.f, 0.f}}}};
                cmd.beginRendering({.renderArea = {{0, 0}, {16, 16}},
                                    .layerCount = 1,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &attachment},
                                   dispatch);
                cmd.draw(primitive == AmdGpu::PrimitiveType::QuadList ? 4 : 3, 1, 0, 0, dispatch);
                cmd.endRendering(dispatch);
                auto copy_barrier = begin;
                copy_barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
                copy_barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
                copy_barrier.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
                copy_barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
                copy_barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
                copy_barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
                cmd.pipelineBarrier2(
                    {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &copy_barrier},
                    dispatch);
                const vk::BufferImageCopy copy{
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageExtent = {16, 16, 1}};
                cmd.copyImageToBuffer(vk::Image(target), vk::ImageLayout::eTransferSrcOptimal,
                                      readback.Handle(), copy, dispatch);
                const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                                              .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                              .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                              .dstAccessMask = vk::AccessFlagBits2::eHostRead};
                cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host}, dispatch);
            });
            scheduler.Finish();
            readback.Invalidate(0, 16 * 16 * 4);
            for (u32 i = 0; i < 16 * 16 * 4; i += 4) {
                const auto *pixel = readback.mapped_data.data() + i;
                if (pixel[0] != 0 || pixel[1] != 255 || pixel[2] != 0 || pixel[3] != 255)
                    std::fprintf(stderr, "%s pixel %u: %u %u %u %u\n",
                                 first ? "prepared" : "inline", i / 4, pixel[0], pixel[1], pixel[2],
                                 pixel[3]);
                assert(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255);
            }
            first = false;
        }
    }
    puts("PASS: triangle, required triangle-strip, generated rect/quad pipelines "
         "render identical pixels after snapshot release");

    // Real compute selection, preparation, key validation, adoption and dispatch.
    alignas(256) std::array<u32, 128> guest_code{};
    guest_code[0] = 0xBEEB03FF;
    guest_code[1] = 1;
    AmdGpu::BinaryInfo binary{};
    binary.signature = AmdGpu::BinaryInfo::signature_ref;
    binary.length = 8;
    auto &regs = gpu.GetCsRegs();
    regs.address = reinterpret_cast<uintptr_t>(guest_code.data()) >> 8;
    regs.num_thread_x.full = 64;
    regs.num_thread_y.full = regs.num_thread_z.full = 1;
    constexpr u32 Words = 128;
    VideoCore::Buffer output{instance, 0, Words * 4, VideoCore::MemoryType::HostCached,
                             "Prepared compute readback"};
    for (const bool prepared : {true, false}) {
        binary.shader_hash = prepared ? 0x12345 : 0x23456;
        std::memcpy(guest_code.data() + 4, &binary, sizeof(binary));
        PrepWorker missing{};
        assert(!cache.PrepareComputePipeline(regs, missing) && missing.missing_shader);
        const auto *info = PipelinePrecompileTest::InstallCompute(cache, regs, *compute, prepared);
        const auto generation = cache.ProgramGeneration();
        if (prepared) {
            auto unseen_permutation = regs;
            unseen_permutation.num_thread_x.full = 32;
            PrepWorker different{};
            assert(!cache.PrepareComputePipeline(unseen_permutation, different) && different.missing_shader);
            PrepWorker worker{};
            assert(cache.PrepareComputePipeline(regs, worker));
            regs.user_data[0] = 999; // Owned queued snapshot must survive changed register data.
        }
        const auto *pipeline = cache.GetComputePipeline();
        if (!prepared) assert(cache.ProgramGeneration() == generation + 1);
        assert(&pipeline->GetStage(Shader::SwStage::Compute) == info);
        PipelinePrecompileTest::CheckComputeAdoption(cache, prepared);
        std::memset(output.mapped_data.data(), 0, Words * 4);
        output.Flush(0, Words * 4);
        scheduler.Record([&, pipeline](vk::CommandBuffer cmd) {
            const vk::DescriptorBufferInfo buffer{output.Handle(), 0, Words * 4};
            const vk::WriteDescriptorSet write{.dstBinding = 0,
                                               .descriptorCount = 1,
                                               .descriptorType = vk::DescriptorType::eStorageBuffer,
                                               .pBufferInfo = &buffer};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle(), dispatch);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline->GetLayout(), 0,
                                     write, dispatch);
            cmd.dispatch(2, 1, 1, dispatch);
            const vk::MemoryBarrier2 barrier{.srcStageMask =
                                                 vk::PipelineStageFlagBits2::eComputeShader,
                                             .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                             .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                             .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, dispatch);
        });
        scheduler.Finish();
        output.Invalidate(0, Words * 4);
        for (u32 i = 0; i < Words; ++i) {
            u32 actual;
            std::memcpy(&actual, output.mapped_data.data() + i * 4, 4);
            assert(actual == (i ^ 0xA5A5u));
        }
    }
    cache.Sync();
    puts("PASS: prepared and required production compute pipelines validate "
         "keys, rebind runtime stages and produce exact GPU readback; caches "
         "merged after drain");
}
