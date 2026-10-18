// SPDX-License-Identifier: GPL-2.0-or-later
// Differential reference: the pre-table Techmo layout, including volume padding.
#include <cstdio>
#include <cstdlib>
#include <random>
#include "video_core/amdgpu/tiling.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/tile.h"
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL layout %d: %s\n",__LINE__,#x);std::_Exit(1); } } while(false)
using namespace VideoCore;
static void Reference(ImageInfo& image) {
    image.guest_size=0;
    for(u32 mip=0;mip<image.resources.levels;++mip) {
        u32 w=image.pitch>>mip,h=image.size.height>>mip;
        if(image.props.is_block) { w=(w+3)/4;h=(h+3)/4; }
        w=std::max(w,1u);h=std::max(h,1u);u32 d=std::max(image.size.depth>>mip,1u);
        if(image.props.is_pow2) { w=std::bit_ceil(w);h=std::bit_ceil(h);d=std::bit_ceil(d); }
        auto& entry=image.mips_layout[mip];
        if(image.array_mode==AmdGpu::ArrayMode::ArrayLinearAligned)
            std::tie(entry.pitch,entry.height,entry.size)=ImageSizeLinearAligned(w,h,image.num_bits,image.num_samples);
        else {
            const u32 thickness=AmdGpu::GetMicroTileThickness(image.array_mode);d+=(-d)&(thickness-1);
            if(AmdGpu::IsMacroTiled(image.array_mode))
                std::tie(entry.pitch,entry.height,entry.size)=ImageSizeMacroTiled(w,h,thickness,image.num_bits,image.num_samples,image.tile_mode,mip,image.alt_tile);
            else std::tie(entry.pitch,entry.height,entry.size)=ImageSizeMicroTiled(w,h,thickness,image.num_bits,image.num_samples);
        }
        if(image.props.is_block) { entry.pitch=std::max(entry.pitch*4,32u);entry.height=std::max(entry.height*4,32u); }
        entry.size*=d*image.resources.layers;entry.offset=image.guest_size;image.guest_size+=entry.size;
    }
}
static void Check(ImageInfo image) {
    auto expected=image;Reference(expected);image.UpdateSize();CHECK(image.guest_size==expected.guest_size);
    for(u32 i=0;i<16;++i) {
        const auto& a=image.mips_layout[i];const auto& b=expected.mips_layout[i];
        CHECK(a.size==b.size && a.offset==b.offset && a.pitch==b.pitch && a.height==b.height);
    }
}
int main() {
    for(u32 bits:{64u,128u}) for(u32 power=0;power<=12;++power) {
        ImageInfo info{};info.props.is_block=1;info.props.is_pow2=1;
        info.pitch=1u<<power;info.size={info.pitch,info.pitch,1};info.num_bits=bits;
        info.tile_mode=AmdGpu::TileMode::Thin1DThin;info.array_mode=AmdGpu::GetArrayMode(info.tile_mode);
        info.resources={power+1,1};Check(info);
        if(power) { --info.resources.levels;Check(info); }
        info.resources.layers=3;Check(info);info.alt_tile=true;Check(info);
    }
    std::mt19937 random(0x5219);
    const std::array modes{AmdGpu::TileMode::Thin1DThin,AmdGpu::TileMode::Thin2DThin,
        AmdGpu::TileMode::Thin3DThin,AmdGpu::TileMode::Thick1DThick,AmdGpu::TileMode::Thick2DThick,
        AmdGpu::TileMode::Thick3DThick,AmdGpu::TileMode::DisplayLinearAligned};
    for(u32 i=0;i<10000;++i) {
        ImageInfo info{};info.tile_mode=modes[random()%modes.size()];info.array_mode=AmdGpu::GetArrayMode(info.tile_mode);
        info.size={1+random()%512,1+random()%512,1+random()%7};info.pitch=info.size.width;
        info.num_bits=8u<<(random()%5);info.num_samples=1u<<(random()%4);
        info.props.is_block=info.tile_mode==AmdGpu::TileMode::Thin1DThin && (random()&1);
        if(info.props.is_block) info.num_bits=(random()&1)?64:128;
        info.props.is_pow2=random()&1;info.alt_tile=random()&1;
        info.resources={1+random()%10,1+random()%4};Check(info);
    }
    puts("PASS: exact BCn table layouts and fallbacks match frozen Techmo reference; 10k mixed linear/micro/macro/thick/volume/sample/array cases");
}
