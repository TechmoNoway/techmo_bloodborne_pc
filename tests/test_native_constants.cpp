// SPDX-License-Identifier: GPL-2.0-or-later
// Emit production shaders and execute their production compute layouts on the GPU.
#include "shader_recompiler/constant_buffer.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/buffer_cache/buffer.h"
#include <array>
#include <cassert>
#include <crtdbg.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

int main(int argc,char** argv) {
    _set_error_mode(_OUT_TO_STDERR); _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    try {
    assert(argc==2); const std::filesystem::path root(argv[1]); std::filesystem::create_directories(root);
    Vulkan::Instance instance{-1,false}; Vulkan::Scheduler scheduler{instance,false};
    const auto device=instance.GetDevice();
    static vk::detail::DynamicLoader loader; vk::detail::DispatchLoaderDynamic dispatch;
    const auto gi=loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto gd=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gi(static_cast<VkInstance>(instance.GetInstance()),"vkGetDeviceProcAddr"));
    dispatch.vkCreatePipelineCache=reinterpret_cast<PFN_vkCreatePipelineCache>(gd(static_cast<VkDevice>(device),"vkCreatePipelineCache"));
    dispatch.vkDestroyPipelineCache=reinterpret_cast<PFN_vkDestroyPipelineCache>(gd(static_cast<VkDevice>(device),"vkDestroyPipelineCache"));
    dispatch.vkCreateShaderModule=reinterpret_cast<PFN_vkCreateShaderModule>(gd(static_cast<VkDevice>(device),"vkCreateShaderModule"));
    dispatch.vkDestroyShaderModule=reinterpret_cast<PFN_vkDestroyShaderModule>(gd(static_cast<VkDevice>(device),"vkDestroyShaderModule"));
    dispatch.vkCmdBindPipeline=reinterpret_cast<PFN_vkCmdBindPipeline>(gd(static_cast<VkDevice>(device),"vkCmdBindPipeline"));
    dispatch.vkCmdPushDescriptorSetKHR=reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(gd(static_cast<VkDevice>(device),"vkCmdPushDescriptorSetKHR"));
    dispatch.vkCmdDispatch=reinterpret_cast<PFN_vkCmdDispatch>(gd(static_cast<VkDevice>(device),"vkCmdDispatch"));
    dispatch.vkCmdPipelineBarrier2=reinterpret_cast<PFN_vkCmdPipelineBarrier2>(gd(static_cast<VkDevice>(device),"vkCmdPipelineBarrier2"));
    const std::array pools{vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer,4},vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer,4}};
    Vulkan::DescriptorHeap heap{instance,scheduler.GetWorkSemaphore(),pools};
    auto native_cache=device.createPipelineCacheUnique({},nullptr,dispatch).value;
    for (bool native : {false,true}) {
        Shader::Info info{}; info.hw_stage=Shader::HwStage::Compute; info.sw_stage=Shader::SwStage::Compute;
        info.srt_info.flattened_bufsize_dw=17; info.flattened_ud_buf.resize(17);
        info.flattened_ud_buf[16]=0x4a2b9c17;
        AmdGpu::Buffer sharp{}; sharp.base_address=0x100000; sharp.num_records=1; sharp.stride=4;
        auto& output=info.buffers.emplace_back(); output.is_written=true; output.used_types=Shader::IR::Type::U32;
        std::memcpy(output.sharp_fetch.immediates.data(),&sharp,sizeof(sharp));
        auto& flat=info.buffers.emplace_back(); flat.buffer_type=Shader::BufferType::Flatbuf; flat.used_types=Shader::IR::Type::U32;
        // A known flattened offset uses a direct load; it needs no DMA read function.
        info.has_readconst=true;
        Shader::RuntimeInfo runtime{}; runtime.Initialize(info.hw_stage,info.sw_stage); runtime.hw.cs.workgroup_size={1,1,1};
        Common::ObjectPool<Shader::IR::Inst> insts; Shader::IR::Block block{insts}; Shader::IR::IREmitter ir{block};
        auto value=ir.ReadConst(ir.CompositeConstruct(ir.Imm32(0u),ir.Imm32(0u)),ir.Imm32(0u));
        value.Inst()->SetFlags(u32{16});
        Shader::IR::BufferInstInfo store{}; store.voffset_enable.Assign(1);
        ir.StoreBufferU32(1,ir.Imm32(0u),ir.Imm32(0u),value,store); ir.Epilogue();
        Shader::IR::Program program{info}; program.blocks.push_back(&block);
        program.syntax_list.push_back({.data={.block=&block},.type=Shader::IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type=Shader::IR::AbstractSyntaxNode::Type::Return});
        Shader::Profile profile{}; profile.supported_spirv=0x00010600; profile.support_int64=true; profile.native_constants=native;
        assert(Shader::UniformConstants(profile,info)==native);
        Shader::Backend::Bindings bindings{};
        const auto code=Shader::Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
        std::ofstream file(root/(native ? "uniform.spv" : "storage.spv"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(code.data()),code.size()*4); file.close();
        auto module=device.createShaderModuleUnique({.codeSize=code.size()*4,.pCode=code.data()},nullptr,dispatch).value;
        Vulkan::ComputePipeline::SerializationSupport data;
        Vulkan::ComputePipeline pipeline{instance,scheduler,heap,profile,*native_cache,{.value=u64(native)+1},info,*module,data,true};
        VideoCore::Buffer constants{instance,0,80,VideoCore::MemoryType::Stream,"Constants fixture"};
        VideoCore::Buffer result{instance,0,16,VideoCore::MemoryType::HostCached,"Constants result"};
        std::memset(constants.mapped_data.data(),0,80);
        std::memcpy(constants.mapped_data.data(),info.flattened_ud_buf.data(),68); constants.Flush(0,80);
        const std::array buffers{vk::DescriptorBufferInfo{result.Handle(),0,16},vk::DescriptorBufferInfo{constants.Handle(),0,80}};
        std::array<vk::WriteDescriptorSet,2> writes{};
        for (u32 i=0;i<2;++i) writes[i]={.dstBinding=i,.descriptorCount=1,
            .descriptorType=i==1 && native ? vk::DescriptorType::eUniformBuffer : vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffers[i]};
        const auto cmd=scheduler.CommandBuffer();
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,pipeline.Handle(),dispatch);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,pipeline.GetLayout(),0,writes,dispatch);
        cmd.dispatch(1,1,1,dispatch);
        const vk::MemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask=vk::AccessFlagBits2::eShaderWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&barrier},dispatch);
        scheduler.Finish(); result.Invalidate(0,4);
        u32 actual{}; std::memcpy(&actual,result.mapped_data.data(),4); assert(actual==0x4a2b9c17);
        profile.native_constants=true; info.srt_info.flattened_bufsize_dw=4097;
        assert(!Shader::UniformConstants(profile,info));
    }
    puts("PASS: production storage/uniform shaders and compute layouts return identical GPU values, padded tail and oversized fallback");
    } catch (const std::exception& error) {
        std::fprintf(stderr,"Constants fixture exception: %s\n",error.what());
        return 1;
    }
}
