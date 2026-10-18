// SPDX-License-Identifier: GPL-2.0-or-later
// Real GPU vertex/index/indirect/count/dispatch readers followed by overwrites.
#include <cassert>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <crtdbg.h>
#include "buffer_access_vert.h"
#include "pipeline_prep_comp.h"
#include "bbport_toggles.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
namespace Vulkan {
struct BufferAccessTest {
    static void Stage(Runtime& runtime,vk::PipelineStageFlags2 expected) {
        assert((runtime.memory_barrier.srcStageMask&expected)==expected);
    }
};
}
int main(int argc,char** argv) {
    _set_error_mode(_OUT_TO_STDERR);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    if(argc>1 && !std::strcmp(argv[1],"--precise"))
        runtime_disabled_optimizations=BbToggle::AccessMemo|BbToggle::CoarseReadTracking;
    using namespace Vulkan;using namespace VideoCore;
    Instance instance{-1,false};Scheduler scheduler{instance,false};Runtime runtime{instance,scheduler};
    const auto device=instance.GetDevice();
    static vk::detail::DynamicLoader loader;vk::detail::DispatchLoaderDynamic d;
    const auto gi=loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto gd=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gi(static_cast<VkInstance>(instance.GetInstance()),"vkGetDeviceProcAddr"));
#define LOAD(name) d.name=reinterpret_cast<PFN_##name>(gd(static_cast<VkDevice>(device),#name))
    LOAD(vkCreateShaderModule);LOAD(vkDestroyShaderModule);LOAD(vkCreateDescriptorSetLayout);LOAD(vkDestroyDescriptorSetLayout);
    LOAD(vkCreatePipelineLayout);LOAD(vkDestroyPipelineLayout);LOAD(vkCreateGraphicsPipelines);LOAD(vkCreateComputePipelines);LOAD(vkDestroyPipeline);
    LOAD(vkCmdBindPipeline);LOAD(vkCmdPushDescriptorSetKHR);LOAD(vkCmdBeginRendering);LOAD(vkCmdEndRendering);
    LOAD(vkCmdBindVertexBuffers);LOAD(vkCmdBindIndexBuffer);LOAD(vkCmdDrawIndirect);LOAD(vkCmdDrawIndexedIndirect);
    LOAD(vkCmdDrawIndirectCount);LOAD(vkCmdDrawIndexedIndirectCount);LOAD(vkCmdDispatchIndirect);LOAD(vkCmdPipelineBarrier2);
#undef LOAD
    const vk::DescriptorSetLayoutBinding binding{0,vk::DescriptorType::eStorageBuffer,1,vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eCompute};
    auto desc=device.createDescriptorSetLayoutUnique({.flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,.bindingCount=1,.pBindings=&binding},nullptr,d).value;
    auto layout=device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*desc},nullptr,d).value;
    auto vs=device.createShaderModuleUnique({.codeSize=sizeof(BUFFER_ACCESS_VERT),.pCode=BUFFER_ACCESS_VERT},nullptr,d).value;
    auto cs=device.createShaderModuleUnique({.codeSize=sizeof(PIPELINE_PREP_COMP),.pCode=PIPELINE_PREP_COMP},nullptr,d).value;
    const vk::PipelineShaderStageCreateInfo stage{.stage=vk::ShaderStageFlagBits::eVertex,.module=*vs,.pName="main"};
    const vk::VertexInputBindingDescription vb{0,4,vk::VertexInputRate::eVertex};
    const vk::VertexInputAttributeDescription attr{0,0,vk::Format::eR32Uint,0};
    const vk::PipelineVertexInputStateCreateInfo input{.vertexBindingDescriptionCount=1,.pVertexBindingDescriptions=&vb,.vertexAttributeDescriptionCount=1,.pVertexAttributeDescriptions=&attr};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{.topology=vk::PrimitiveTopology::ePointList};
    const vk::Viewport viewport{0,0,1,1,0,1};const vk::Rect2D scissor{{0,0},{1,1}};
    const vk::PipelineViewportStateCreateInfo view{.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    const vk::PipelineRasterizationStateCreateInfo raster{.rasterizerDiscardEnable=true,.polygonMode=vk::PolygonMode::eFill,.lineWidth=1};
    const vk::PipelineMultisampleStateCreateInfo msaa{.rasterizationSamples=vk::SampleCountFlagBits::e1};
    const vk::PipelineColorBlendStateCreateInfo blend{};const vk::PipelineRenderingCreateInfo rendering{};
    auto graphics=device.createGraphicsPipelineUnique({}, {.pNext=&rendering,.stageCount=1,.pStages=&stage,.pVertexInputState=&input,.pInputAssemblyState=&assembly,.pViewportState=&view,.pRasterizationState=&raster,.pMultisampleState=&msaa,.pColorBlendState=&blend,.layout=*layout},nullptr,d).value;
    auto compute=device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*cs,.pName="main"},.layout=*layout},nullptr,d).value;
    Buffer data{instance,0,4096,MemoryType::DeviceLocal,"Access data"};
    Buffer output{instance,0,256,MemoryType::DeviceLocal,"Access output"};
    Buffer readback{instance,0,2048,MemoryType::HostCached,"Access verification"};
    const auto access=[&](u64 offset,u64 size,vk::PipelineStageFlagBits2 st,vk::AccessFlagBits2 ac) {
        runtime.AccessBuffer(&data,offset,size,st,ac);
        assert(runtime.IsBufferAccessed(&data,offset,size,true));
        BufferAccessTest::Stage(runtime,st);
    };
    for(auto [st,ac]:{std::pair{vk::PipelineStageFlagBits2::eVertexAttributeInput,vk::AccessFlagBits2::eVertexAttributeRead},
                     std::pair{vk::PipelineStageFlagBits2::eIndexInput,vk::AccessFlagBits2::eIndexRead},
                     std::pair{vk::PipelineStageFlagBits2::eDrawIndirect,vk::AccessFlagBits2::eIndirectCommandRead}}) {
        runtime.AccessBuffer(&data,0,0,st,ac);assert(!runtime.IsBufferAccessed(&data,0,0,true));
        access(128,4,st,ac);assert(!runtime.IsBufferAccessed(&data,128,4));runtime.FlushBarriers();
        assert(!runtime.IsBufferAccessed(&data,128,4,true));
    }
    for(u32 iteration=0;iteration<16;++iteration) {
        const bool indexed=(iteration&1)!=0, counted=(iteration&2)!=0;
        const bool empty=(iteration&4)!=0 && counted;
        std::array<u32,128> words{};
        for(u32 i=0;i<3;++i) words[i]=iteration*100+i+11;
        words[16]=2;words[17]=1;words[18]=0;
        words[32]=3;words[33]=1;words[34]=0;words[35]=0;words[36]=0;
        words[64]=empty?0:1;
        runtime.UpdateBuffer(&data,0,{reinterpret_cast<const u8*>(words.data()),sizeof(words)});
        runtime.FillBuffer(&output,0,256,0);
        // An earlier output reuse may already have flushed the data producer.
        runtime.FlushBarriers();
        scheduler.Record([&,indexed,counted](vk::CommandBuffer cmd) {
            const vk::DescriptorBufferInfo info{output.Handle(),0,256};
            const vk::WriteDescriptorSet write{.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&info};
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics,*graphics,d);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,*layout,0,write,d);
            const vk::Buffer handle=data.Handle();const vk::DeviceSize offset=0;
            cmd.bindVertexBuffers(0,1,&handle,&offset,d);
            if(indexed) cmd.bindIndexBuffer(data.Handle(),64,vk::IndexType::eUint32,d);
            cmd.beginRendering({.renderArea={{0,0},{1,1}},.layerCount=1},d);
            if(indexed && counted) cmd.drawIndexedIndirectCount(data.Handle(),128,data.Handle(),256,1,20,d);
            else if(indexed) cmd.drawIndexedIndirect(data.Handle(),128,1,20,d);
            else if(counted) cmd.drawIndirectCount(data.Handle(),128,data.Handle(),256,1,16,d);
            else cmd.drawIndirect(data.Handle(),128,1,16,d);
            cmd.endRendering(d);
        });
        access(0,12,vk::PipelineStageFlagBits2::eVertexAttributeInput,vk::AccessFlagBits2::eVertexAttributeRead);
        access(128,indexed?20:16,vk::PipelineStageFlagBits2::eDrawIndirect,vk::AccessFlagBits2::eIndirectCommandRead);
        if(indexed) access(64,12,vk::PipelineStageFlagBits2::eIndexInput,vk::AccessFlagBits2::eIndexRead);
        if(counted) access(256,4,vk::PipelineStageFlagBits2::eDrawIndirect,vk::AccessFlagBits2::eIndirectCommandRead);
        runtime.AccessBuffer(&output,0,256,vk::PipelineStageFlagBits2::eVertexShader,vk::AccessFlagBits2::eShaderWrite);
        // Overwrite arguments/vertices immediately, with no host wait between consumers.
        runtime.FillBuffer(&data,0,4096,0);
        const vk::BufferCopy copy{0,iteration*16,16};runtime.CopyBuffer(&output,&readback,std::span{&copy,1});
    }
    for(u32 iteration=0;iteration<4;++iteration) {
        const std::array<u32,3> groups{1,1,1};
        runtime.UpdateBuffer(&data,320,{reinterpret_cast<const u8*>(groups.data()),sizeof(groups)});
        runtime.FillBuffer(&output,0,256,0);runtime.FlushBarriers();
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const vk::DescriptorBufferInfo info{output.Handle(),0,256};
            const vk::WriteDescriptorSet write{.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&info};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*compute,d);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*layout,0,write,d);
            cmd.dispatchIndirect(data.Handle(),320,d);
        });
        access(320,12,vk::PipelineStageFlagBits2::eDrawIndirect,vk::AccessFlagBits2::eIndirectCommandRead);
        runtime.AccessBuffer(&output,0,256,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite);
        runtime.FillBuffer(&data,0,4096,0);
        const vk::BufferCopy copy{0,512+iteration*256,256};runtime.CopyBuffer(&output,&readback,std::span{&copy,1});
    }
    scheduler.Finish();readback.Invalidate(0,2048);
    const auto* values=reinterpret_cast<const u32*>(readback.mapped_data.data());
    for(u32 iteration=0;iteration<16;++iteration) for(u32 i=0;i<3;++i)
        assert(values[iteration*4+i]==(((iteration&4) && (iteration&2))?0:iteration*100+i+11));
    for(u32 iteration=0;iteration<4;++iteration) for(u32 i=0;i<64;++i)
        assert(values[128+iteration*64+i]==(i^0xA5A5u));
    puts("PASS: typed read hazards, empty ranges, real GPU vertex/index/indirect/count/dispatch consumption before immediate overwrite; 20 queued readbacks");
}
