// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/page_manager.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL stable arena line %d: %s\n",__LINE__,#x); std::_Exit(1); } } while(false)
namespace VideoCore {
struct StableArenaTest {
    static void Run(BufferCache& cache, Vulkan::Runtime& runtime, Vulkan::Scheduler& scheduler) {
        const u64 block=cache.block_size;
        const auto get=[&](u64 address,u64 size) { return cache.GetArena(address/block,(address+size-1)/block); };
        const u64 Boundary=0x1000000000ULL + (u64{1} << cache.arena_page_bits);
        const Buffer* old=get(Boundary-block,block);
        const auto handle=old->Handle();
        const auto address=old->BufferDeviceAddress()+old->Offset(Boundary-block);
        const Buffer* right=get(Boundary,block);
        CHECK(old!=right && old->Handle()==handle);
        CHECK(address==get(Boundary-block,block)->BufferDeviceAddress()+old->Offset(Boundary-block));
        for (u64 base : {0x0800000000ULL,0x2000000000ULL,0x7f00000000ULL,0xfb00000000ULL}) {
            const auto* before=get(base,block);
            get(base-block,2*block);
            CHECK(get(base,block)==before);
        }
        // Bind actual physical pages at the boundary, write through the old
        // buffer, then read them through the range requested across it.
        const u64 first=Boundary/block-1;
        const auto backing=cache.AllocateResidency(2*block);
        BufferCache::Backing resident;
        resident.start=first;resident.end=first+2;
        resident.memory=backing.first;resident.offset=backing.second/block;
        cache.resident_ranges.Add(resident);
        cache.BindResidentAliases(first,first+2,backing.first,backing.second);
        Buffer output{cache.instance,0,2*block,MemoryType::HostCached};
        runtime.FillBuffer(old,old->Offset(Boundary-block),block,0x12345678);
        runtime.FillBuffer(right,right->Offset(Boundary),block,0xaabbccdd);
        cache.gpu_modified_ranges.Add(Boundary-block,2*block);
        const Buffer* cross=get(Boundary-block,2*block);
        CHECK(old->Handle()==handle && get(Boundary-block,block)==old && get(Boundary,block)==right);
        CHECK(runtime.IsBufferAccessed(cross,cross->Offset(Boundary-block),2*block,false));
        const vk::BufferCopy copy{cross->Offset(Boundary-block),0,2*block};
        runtime.CopyBuffer(cross,&output,std::span{&copy,1});
        scheduler.Finish();output.Invalidate(0,2*block);
        for(u64 i=0;i<2*block;i+=4) {
            u32 value;std::memcpy(&value,output.mapped_data.data()+i,4);
            CHECK(value==(i<block?0x12345678u:0xaabbccddu));
        }
        // A window first requested after physical backing exists must inherit
        // that backing too, without changing either canonical address.
        const auto* late=get(Boundary-(u64{1} << cache.arena_page_bits)-block,block);
        const vk::BufferCopy late_copy{late->Offset(Boundary-block),0,block};
        runtime.CopyBuffer(late,&output,std::span{&late_copy,1});
        scheduler.Finish();output.Invalidate(0,block);
        for(u64 i=0;i<block;i+=4) {
            u32 value;std::memcpy(&value,output.mapped_data.data()+i,4);
            CHECK(value==0x12345678u);
        }
        CHECK(old->Handle()==handle && old->BufferDeviceAddress()+old->Offset(Boundary-block)==address);
        std::puts("PASS: stable buffer/BDA across adjacent pages and distant ranges, alias hazards, actual sparse GPU roundtrip");
    }
    static void SmallPages(BufferCache& cache) {
        CHECK(cache.arenas.empty());
        cache.arena_page_bits=30;
        cache.blocks_per_arena_page_shift=30-cache.block_shift;
        cache.address_space.assign(1024,nullptr);
    }
};
}
struct Caches {
    VideoCore::PageManager pages{nullptr}; VideoCore::BufferCache buffers; VideoCore::TextureCache textures;
    Caches(Vulkan::Instance& instance,Vulkan::Scheduler& scheduler,Vulkan::Runtime& runtime)
        : buffers{instance,scheduler,runtime,nullptr,textures,pages},
          textures{instance,scheduler,runtime,nullptr,buffers,pages} {}
};
int main(int argc, char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    using namespace VideoCore;
    _putenv_s("BB_GUEST_IN_PLACE","0");
    Vulkan::Instance instance{-1,false}; Vulkan::Scheduler scheduler{instance,true};
    Vulkan::Runtime runtime{instance,scheduler}; Caches caches{instance,scheduler,runtime};
    auto& cache=caches.buffers;
    if (argc==2 && !std::strcmp(argv[1],"--small-pages")) StableArenaTest::SmallPages(cache);
    scheduler.SetSubmitCallback([&](Vulkan::SubmitInfo& info) { runtime.FlushBarriers();cache.SubmitPendingArenaBinds(info); });
    StableArenaTest::Run(cache,runtime,scheduler);
    scheduler.Finish();
}
