// SPDX-License-Identifier: GPL-3.0-or-later
// Run the actual presentation pass headlessly; no game image, save or display change.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "hdr_output.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include <vk_mem_alloc.h>

static void Check(bool pass, const char* label) {
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
int main() {
    using namespace Vulkan;
    Instance instance(0, false);
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic d;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    d.init(instance.GetInstance()); d.init(instance.GetDevice());
    const auto device = instance.GetDevice();
    Scheduler scheduler(instance);
    constexpr u32 W=16, H=4, N=W*H;
    VideoCore::UniqueImage source(device, instance.GetAllocator());
    source.Create({.imageType=vk::ImageType::e2D, .format=vk::Format::eR32G32B32A32Sfloat,
        .extent={W,H,1}, .mipLevels=1, .arrayLayers=1, .samples=vk::SampleCountFlagBits::e1,
        .tiling=vk::ImageTiling::eOptimal, .usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst});
    auto source_view=device.createImageViewUnique({.image=vk::Image(source),
        .viewType=vk::ImageViewType::e2D, .format=vk::Format::eR32G32B32A32Sfloat,
        .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}},nullptr,d).value;
    VkBuffer staging{}; VmaAllocation allocation{}; VmaAllocationInfo info{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=N*16+N*4,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT|VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage=VMA_MEMORY_USAGE_AUTO,.requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    Check(vmaCreateBuffer(instance.GetAllocator(),&bi,&ac,&staging,&allocation,&info)==VK_SUCCESS,"Mapped readback allocation");
    const std::array<std::array<float,3>,8> values{{{0,0,0},{1,1,1},{.5f,.5f,.5f},{.1f,.1f,.1f},
        {1,0,0},{0,1,0},{0,0,1},{.75f,.5f,.25f}}};
    for(u32 p=0;p<N;++p) {
        auto* v=static_cast<float*>(info.pMappedData)+p*4;
        std::copy(values[p%8].begin(),values[p%8].end(),v);v[3]=1;
    }
    const auto transition=[&](vk::CommandBuffer cmd,vk::Image image,vk::ImageLayout old,vk::ImageLayout next,
                              vk::PipelineStageFlags2 stage,vk::AccessFlags2 access) {
        const vk::ImageMemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,.dstStageMask=stage,.dstAccessMask=access,
            .oldLayout=old,.newLayout=next,.image=image,
            .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b},d);
    };
    auto cmd=scheduler.CommandBuffer();
    transition(cmd,source,vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal,
        vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferWrite);
    cmd.copyBufferToImage(staging,source,vk::ImageLayout::eTransferDstOptimal,
        vk::BufferImageCopy{.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={W,H,1}},d);
    transition(cmd,source,vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::PipelineStageFlagBits2::eFragmentShader,vk::AccessFlagBits2::eShaderRead);
    scheduler.Finish();
    for (const bool ten_bit : {false,true}) {
        const auto format=ten_bit?vk::Format::eA2B10G10R10UnormPack32:vk::Format::eR8G8B8A8Unorm;
        VideoCore::UniqueImage output(device,instance.GetAllocator());
        output.Create({.imageType=vk::ImageType::e2D,.format=format,.extent={W,H,1},.mipLevels=1,.arrayLayers=1,
            .samples=vk::SampleCountFlagBits::e1,.tiling=vk::ImageTiling::eOptimal,
            .usage=vk::ImageUsageFlagBits::eColorAttachment|vk::ImageUsageFlagBits::eTransferSrc});
        auto view=device.createImageViewUnique({.image=output,.viewType=vk::ImageViewType::e2D,.format=format,
            .subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}},nullptr,d).value;
        HostPasses::PostProcessingPass pass;pass.Create(device,format);
        Frame frame{.width=W,.height=H,.image=output,.image_view=*view};
        for (u32 mode=0;mode<(ten_bit?4u:2u);++mode) {
            HostPasses::PostProcessingPass::Settings settings;
            settings.sdr_to_hdr=ten_bit?1:0;
            settings.srgb_input=mode&1;
            settings.hdr_expand=(mode&2)?1.f:0.f;
            cmd=scheduler.CommandBuffer();
            transition(cmd,output,vk::ImageLayout::eUndefined,vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput,vk::AccessFlagBits2::eColorAttachmentWrite);
            pass.Render(cmd,*source_view,{W,H},frame,settings);
            transition(cmd,output,vk::ImageLayout::eGeneral,vk::ImageLayout::eTransferSrcOptimal,
                vk::PipelineStageFlagBits2::eTransfer,vk::AccessFlagBits2::eTransferRead);
            cmd.copyImageToBuffer(output,vk::ImageLayout::eTransferSrcOptimal,staging,
                vk::BufferImageCopy{.bufferOffset=N*16,.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={W,H,1}},d);
            const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host},d);
            scheduler.Finish();
            const auto* packed=reinterpret_cast<const u32*>(static_cast<const char*>(info.pMappedData)+N*16);
            for(u32 p=0;p<N;++p) {
                auto expected=values[p%8];
                if(settings.srgb_input)for(auto& c:expected)c=HdrOutput::SrgbToLinear(c);
                if(ten_bit) expected=HdrOutput::Convert(expected,203,1000,settings.hdr_expand!=0);
                else for(auto& c:expected)c=c<.0031308f?12.92f*c:1.055f*std::pow(c,1.f/2.4f)-.055f;
                for(u32 c=0;c<3;++c) {
                    const float actual=ten_bit?float((packed[p]>>(10*c))&1023)/1023:float((packed[p]>>(8*c))&255)/255;
                    if(std::abs(actual-expected[c])> (ten_bit?.002f:.005f)) {
                        std::fprintf(stderr,"Post-process: %u-bit mode %u pixel %u channel %u got %.6f expected %.6f\n",ten_bit?10:8,mode,p,c,actual,expected[c]);
                        Check(false,"Production shader readback");
                    }
                }
            }
        }
    }
    vmaDestroyBuffer(instance.GetAllocator(),staging,allocation);
    std::puts("PASS: production post-process GPU readback: SDR regression, 10-bit PQ, primaries, decoding, expansion");
}
