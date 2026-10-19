// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from DarkIzuku dceac3f478. Real GPU overlay preserving padded guest texels.
#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/tile_manager.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL tiled overlay line %d: %s\\n",__LINE__,#x); std::_Exit(1); } } while (false)
#include <cstring>
#include <vector>
#include "bbport_toggles.h"

static void ScratchReuse(Vulkan::Instance& instance,Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime,VideoCore::TileManager& tiles,
                         Common::SlotVector<VideoCore::ImageView>& views) {
    using namespace VideoCore;
    std::vector<std::unique_ptr<Image>> images;
    std::vector<std::unique_ptr<Buffer>> sources;
    const auto before=BbStats::detile_scratch_reuses.load();
    for(u32 pass=0;pass<16;++pass) {
        const u32 width=128u<<std::min(pass,2u);
        ImageInfo info{};info.type=AmdGpu::ImageType::Color2D;info.pixel_format=vk::Format::eR8G8B8A8Unorm;
        info.size={width,width,1};info.pitch=width;info.num_bits=32;info.resources={1,1};
        info.props.is_tiled=1;info.tile_mode=AmdGpu::TileMode::Thin1DThin;
        info.array_mode=AmdGpu::ArrayMode::Array1DTiledThin1;info.UpdateSize();
        auto& input=sources.emplace_back(std::make_unique<Buffer>(instance,0,info.guest_size,MemoryType::HostUncached));
        const u32 color=0xff000000u+pass*0x010d17u;
        for(u64 i=0;i<info.guest_size;i+=4) std::memcpy(input->mapped_data.data()+i,&color,4);
        input->Flush(0,info.guest_size);
        auto& image=images.emplace_back(std::make_unique<Image>(instance,runtime,views,info));
        const auto [linear,offset]=tiles.DetileImage(input.get(),0,info);
        const vk::BufferImageCopy copy{.bufferOffset=offset,.bufferRowLength=width,.bufferImageHeight=width,
            .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={width,width,1}};
        runtime.UploadImage(image.get(),linear,std::span{&copy,1});
        // No finish here: all sixteen reads must survive scratch overwrite/growth.
    }
    for(u32 pass=0;pass<images.size();++pass) {
        const auto& info=images[pass]->info;
        Buffer output(instance,0,info.guest_size,MemoryType::HostCached);
        const vk::BufferImageCopy copy{.bufferRowLength=info.pitch,.bufferImageHeight=info.size.height,
            .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={info.size.width,info.size.height,1}};
        runtime.DownloadImage(images[pass].get(),&output,std::span{&copy,1});
        scheduler.Finish();output.Invalidate(0,info.guest_size);
        for(u64 i=0;i<info.guest_size;i+=4) {
            u32 actual;std::memcpy(&actual,output.mapped_data.data()+i,4);
            CHECK(actual==0xff000000u+pass*0x010d17u);
        }
    }
    const bool reuse=!(std::getenv("BB_DETILE_SCRATCH_REUSE") && std::getenv("BB_DETILE_SCRATCH_REUSE")[0]=='0');
    CHECK(BbStats::detile_scratch_reuses.load()-before==(reuse?13u:0u));
    std::puts("PASS: sixteen queued detile uploads keep distinct pixels across scratch reuse/growth and deferred retirement");
}

int main() {
    using namespace VideoCore;
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance, true);
    Vulkan::Runtime runtime(instance, scheduler);
    StreamBuffer uniforms(instance, scheduler, MemoryType::HostUncached, 1024*1024);
    TileManager tiles(instance, scheduler, runtime, uniforms);
    Common::SlotVector<ImageView> views;
    ScratchReuse(instance,scheduler,runtime,tiles,views);
    struct Format { vk::Format format; u32 bits; };
    const Format formats[]={{vk::Format::eR8Unorm,8}, {vk::Format::eR16Uint,16},
        {vk::Format::eR8G8B8A8Unorm,32}, {vk::Format::eR16G16B16A16Sfloat,64},
        {vk::Format::eR32G32B32A32Sfloat,128}};
    for (const auto& format : formats) {
    for (const auto mode : {AmdGpu::TileMode::Thin1DThin, AmdGpu::TileMode::Thin2DThin,
                           AmdGpu::TileMode::Display1DThin, AmdGpu::TileMode::Display2DThin}) {
    ImageInfo info{};
    info.type = AmdGpu::ImageType::Color2D;
    info.pixel_format = format.format;
    info.size = {100, 50, 1};
    info.pitch = 128;
    info.num_bits = format.bits;
    const u32 texel_bytes=format.bits/8;
    const bool macro=mode==AmdGpu::TileMode::Thin2DThin || mode==AmdGpu::TileMode::Display2DThin;
    const u32 levels=macro ? 1 : 2;
    info.resources = {levels, 2};
    info.props.is_tiled = 1;
    info.tile_mode = mode;
    info.array_mode = macro ? AmdGpu::ArrayMode::Array2DTiledThin1 : AmdGpu::ArrayMode::Array1DTiledThin1;
    info.UpdateSize(); // Same alignment/mip calculation as actual game surfaces.
    std::printf("Writeback case: %u bits, tile %u, %u bytes\n",format.bits,u32(mode),info.guest_size);
    std::fflush(stdout);
    std::vector<vk::BufferImageCopy> copies;
    for (u32 mip=0; mip<levels; ++mip) {
        const auto& layout=info.mips_layout[mip];
        copies.push_back({.bufferOffset=layout.offset, .bufferRowLength=layout.pitch,
            .bufferImageHeight=layout.height,
            .imageSubresource={vk::ImageAspectFlagBits::eColor,mip,0,2},
            .imageExtent={100u>>mip,50u>>mip,1}});
    }
    Image image(instance,runtime,views,info);
    Buffer upload(instance,0,info.guest_size,MemoryType::HostUncached);
    Buffer surface(instance,0,info.guest_size,MemoryType::HostCached);
    Buffer readback(instance,0,info.guest_size,MemoryType::HostCached);
    CHECK(!upload.mapped_data.empty() && !surface.mapped_data.empty());
    for (unsigned pass=0; pass<8; ++pass) {
        // Different pixels each pass exercise scheduler/staging reuse and avoid a
        // constant-colour false positive. Guest padding has a separate sentinel.
        std::vector<u8> expected(info.guest_size,0xa5);
        std::memset(surface.mapped_data.data(),0xa5,info.guest_size);
        for (u32 mip=0; mip<levels; ++mip) {
            const auto& m=info.mips_layout[mip];
            for (u32 layer=0; layer<2; ++layer)
                for (u32 y=0; y<(50u>>mip); ++y)
                    for (u32 x=0; x<(100u>>mip); ++x)
                        for (u32 c=0; c<texel_bytes; ++c) {
                            const u32 at=m.offset+((layer*m.height+y)*m.pitch+x)*texel_bytes+c;
                            expected[at]=u8(x*7+y*11+layer*43+mip*17+pass*13+c);
                        }
        }
        std::memcpy(upload.mapped_data.data(),expected.data(),info.guest_size);
        upload.Flush(0,info.guest_size);
        surface.Flush(0,info.guest_size);
        runtime.UploadImage(&image,&upload,copies);
        auto overlay=copies; // TileImageOver adjusts copy offsets, caller owns them
        const auto overlay_storage=tiles.TileImageOver(image,overlay,&surface,0);
        const auto [linear,offset]=tiles.DetileImage(&surface,0,info);
        const vk::BufferCopy copy{offset,0,info.guest_size};
        runtime.CopyBuffer(linear,&readback,std::span{&copy,1});
        runtime.FlushBarriers();
        scheduler.Record([](vk::CommandBuffer cmd) {
            const vk::MemoryBarrier2 barrier{
                .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eHost,
                .dstAccessMask=vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&barrier});
        });
        scheduler.Finish();
        readback.Invalidate(0,info.guest_size);
        for (u32 i=0; i<info.guest_size; ++i) {
            if (readback.mapped_data[i]!=expected[i]) {
                std::fprintf(stderr,"Mismatch: pass %u byte %u, got %u expected %u\n",
                    pass,i,readback.mapped_data[i],expected[i]);
                CHECK(false);
            }
        }
        runtime.TickFrame();
    }
    }
    }
    std::puts("PASS: Vulkan tiled writeback, 8/16/32/64/128 bits, micro/macro tiles, padded rows, two mips/layers and eight reuse cycles");
}
