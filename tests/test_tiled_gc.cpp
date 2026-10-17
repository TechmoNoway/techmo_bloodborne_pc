// SPDX-License-Identifier: GPL-3.0-or-later
// Real Vulkan/cache collection over disposable guest memory. Only the OS page boundary is fake.
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include "bbport_toggles.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL tiled GC line %d: %s\n",__LINE__,#x); std::_Exit(1); } } while (false)
#define runtime_memory_region unused_region
#define runtime_memory_clamp unused_clamp
#define runtime_memory_set_write_watch unused_watch
#define runtime_memory_gpu_protect unused_protect
#define runtime_memory_read_backing unused_read
#define runtime_memory_write_backing unused_write
#define runtime_memory_write_backing_if_generation unused_checked_write
#define runtime_memory_generation unused_generation
#include "test_gpu_runtime_stubs.cpp"
#undef runtime_memory_region
#undef runtime_memory_clamp
#undef runtime_memory_set_write_watch
#undef runtime_memory_gpu_protect
#undef runtime_memory_read_backing
#undef runtime_memory_write_backing
#undef runtime_memory_write_backing_if_generation
#undef runtime_memory_generation
constexpr uintptr_t Guest=0x1000000000ull;
constexpr u64 GuestSize=32ull<<20;
static uint64_t epoch;
static bool mapped=true,recycle_on_store;
extern "C" {
const uint64_t* runtime_memory_generation() { return &epoch; }
int runtime_memory_region(uintptr_t address,uintptr_t* start,uintptr_t* end,int* present) {
    CHECK(address>=Guest && address<Guest+GuestSize);
    *start=Guest; *end=Guest+GuestSize; *present=mapped; return 1;
}
uint64_t runtime_memory_clamp(uintptr_t address,uint64_t size) {
    if (!mapped || address<Guest || address>Guest+GuestSize) return 0;
    return std::min<u64>(size,Guest+GuestSize-address);
}
void runtime_memory_set_write_watch(uintptr_t,uint64_t,int) {}
void runtime_memory_gpu_protect(uintptr_t,uint64_t,int,int) {}
void runtime_memory_read_backing(uintptr_t address,void* data,uint64_t size) {
    CHECK(runtime_memory_clamp(address,size)==size);
    std::memcpy(data,reinterpret_cast<void*>(address),size);
}
int runtime_memory_write_backing(uintptr_t address,const void* data,uint64_t size) {
    CHECK(runtime_memory_clamp(address,size)==size);
    std::memcpy(reinterpret_cast<void*>(address),data,size); return 1;
}
int runtime_memory_write_backing_if_generation(uintptr_t address,const void* data,uint64_t size,uint64_t generation) {
    if (recycle_on_store) __atomic_add_fetch(&epoch,2,__ATOMIC_RELEASE);
    if (generation!=__atomic_load_n(&epoch,__ATOMIC_ACQUIRE) || runtime_memory_clamp(address,size)!=size) return 0;
    return runtime_memory_write_backing(address,data,size);
}
}
namespace VideoCore {
struct PageManager::Impl {};
PageManager::PageManager(Vulkan::Rasterizer*) {}
PageManager::~PageManager()=default;
void PageManager::ReportFaultSites() {}
void PageManager::NoteImageFault(VAddr,u64,u32,u32,u32,u32) { std::abort(); }
void PageManager::OnGpuMap(VAddr,size_t) {}
void PageManager::OnGpuUnmap(VAddr,size_t) {}
template<bool track> void PageManager::UpdatePageWatchers(VAddr,u64) const {}
template<bool track,bool read> void PageManager::UpdatePageWatchersForRegion(VAddr,RegionBits&) const {}
template void PageManager::UpdatePageWatchers<true>(VAddr,u64) const;
template void PageManager::UpdatePageWatchers<false>(VAddr,u64) const;
template void PageManager::UpdatePageWatchersForRegion<true,true>(VAddr,RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<true,false>(VAddr,RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false,true>(VAddr,RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false,false>(VAddr,RegionBits&) const;

struct TiledWritebackTest {
    static ImageId Make(TextureCache& cache,VAddr address,u32 width,u32 color) {
        ImageInfo info{}; info.guest_address=address; info.type=AmdGpu::ImageType::Color2D;
        info.pixel_format=vk::Format::eR8G8B8A8Unorm; info.size={width,width,1};
        info.pitch=width; info.num_bits=32; info.resources={1,1}; info.props.is_tiled=1;
        info.tile_mode=AmdGpu::TileMode::Thin1DThin;
        info.array_mode=AmdGpu::ArrayMode::Array1DTiledThin1; info.UpdateSize();
        const auto id=cache.slot_images.insert(cache.instance,cache.runtime,cache.slot_image_views,info);
        cache.RegisterImage(id); cache.TrackImage(id);
        auto& image=cache.slot_images[id];
        Buffer upload(cache.instance,0,info.guest_size,MemoryType::HostUncached);
        for (u64 offset=0;offset<info.guest_size;offset+=4) std::memcpy(upload.mapped_data.data()+offset,&color,4);
        std::memset(reinterpret_cast<void*>(address),0xa5,info.guest_size);
        upload.Flush(0,info.guest_size);
        const vk::BufferImageCopy copy{.bufferRowLength=width,.bufferImageHeight=width,
            .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={width,width,1}};
        cache.runtime.UploadImage(&image,&upload,std::span{&copy,1});
        cache.scheduler.Finish();
        image.tick_accessed_last=cache.scheduler.CurrentTick()-1;
        image.flags &= ~ImageFlagBits::Dirty; image.flags |= ImageFlagBits::GpuModified;
        CHECK(image.IsTracked() && image.tick_accessed_last);
        return id;
    }
    static void Safety(TextureCache& cache) {
        const auto id=Make(cache,Guest,128,0xff123456);
        auto& image=cache.slot_images[id];
        const ImageInfo saved=image.info;
        const auto reject=[&](auto change) { change(image.info); CHECK(!cache.WriteBackTiledImage(id)); image.info=saved; };
        reject([](auto& info) { info.meta_info.cmask_addr=1; });
        reject([](auto& info) { info.meta_info.fmask_addr=1; });
        reject([](auto& info) { info.meta_info.htile_addr=1; });
        reject([](auto& info) { info.props.is_depth=1; });
        reject([](auto& info) { info.num_samples=2; });
        reject([](auto& info) { info.props.is_block=1; });
        reject([](auto& info) { info.props.is_volume=1; });
        reject([](auto& info) { info.pixel_format=vk::Format::eR16Uint; });
        reject([](auto& info) { info.guest_size=17u<<20; });
        reject([](auto& info) { info.mips_layout[0].pitch=0; });
        image.scene_proxy=true; CHECK(!cache.WriteBackTiledImage(id)); image.scene_proxy=false;
        const auto tick=image.tick_accessed_last;
        image.tick_accessed_last=cache.scheduler.CurrentTick();
        CHECK(!cache.WriteBackTiledImage(id)); image.tick_accessed_last=tick;
        cache.UntrackImage(id); CHECK(!cache.WriteBackTiledImage(id)); cache.TrackImage(id);
        mapped=false; __atomic_add_fetch(&epoch,2,__ATOMIC_RELEASE);
        CHECK(!cache.WriteBackTiledImage(id));
        mapped=true; __atomic_add_fetch(&epoch,2,__ATOMIC_RELEASE);
        const auto alias=Make(cache,Guest,128,0xff654321);
        CHECK(!cache.WriteBackTiledImage(id)); cache.FreeImage(alias); cache.scheduler.Finish();
        recycle_on_store=true; CHECK(!cache.WriteBackTiledImage(id)); recycle_on_store=false;
        CHECK(*reinterpret_cast<u32*>(Guest)==0xa5a5a5a5 && image.IsTracked());
        CHECK(True(image.flags&ImageFlagBits::GpuModified));
        CHECK(cache.gc_tiled_attempts==1 && cache.gc_tiled_writebacks==0);
        CHECK(cache.WriteBackTiledImage(id));
        CHECK(cache.gc_tiled_writebacks==1);
        for (u64 offset=0;offset<saved.guest_size;offset+=4) {
            u32 value; std::memcpy(&value,reinterpret_cast<void*>(Guest+offset),4); CHECK(value==0xff123456);
        }
        cache.FreeImage(id); cache.scheduler.Finish();
        std::puts("PASS: real cache rejects metadata/depth/MSAA/proxy/untracked/in-flight/unmapped/aliased images; stale readback keeps its GPU owner");
    }
    static void Collect(TextureCache& cache,bool enabled,const char* file) {
        constexpr unsigned Count=16;
        for (unsigned i=0;i<Count;++i) Make(cache,Guest+(u64(i)<<20),512,0xff000000u+i*0x070d13u);
        std::ofstream output(file); CHECK(bool(output));
        output<<"pass,enabled,elapsed_ms,image_bytes,evictions,tiled_writebacks,tiled_bytes,tiled_total_ms,tiled_max_ms,driver_usage_bytes\n";
        std::vector<double> timings;
        for (unsigned pass=0;pass<Count;++pass) {
            cache.gc_tick+=1000; cache.gc_tick_at_second.fill(cache.gc_tick-100);
            cache.memory_sample_time={};
            const auto attempts=cache.gc_tiled_attempts;
            const auto start=std::chrono::steady_clock::now();
            cache.GarbageCollectImages(); cache.scheduler.Finish();
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            CHECK(cache.gc_tiled_attempts-attempts<=1);
            timings.push_back(ms);
            output<<pass<<','<<enabled<<','<<ms<<','<<BbStats::vk_image_bytes.load()<<','<<cache.gc_total_evictions<<','
                  <<cache.gc_tiled_writebacks<<','<<cache.gc_tiled_bytes<<','<<cache.gc_tiled_ms<<','<<cache.gc_tiled_max_ms<<','
                  <<cache.instance.GetDeviceMemoryUsage()<<'\n';
        }
        CHECK(cache.gc_tiled_writebacks==(enabled?Count:0));
        CHECK(cache.gc_total_evictions==(enabled?Count:0));
        // A real next frame advances retirement after the final image was unregistered.
        cache.scheduler.Record([](vk::CommandBuffer command) {
            const vk::MemoryBarrier2 barrier{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
                .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask=vk::AccessFlagBits2::eMemoryRead};
            command.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&barrier});
        });
        cache.scheduler.Finish(); cache.scheduler.PopPendingOperations();
        output<<"retired,"<<enabled<<",0,"<<BbStats::vk_image_bytes.load()<<','<<cache.gc_total_evictions<<','
              <<cache.gc_tiled_writebacks<<','<<cache.gc_tiled_bytes<<','<<cache.gc_tiled_ms<<','<<cache.gc_tiled_max_ms<<','
              <<cache.instance.GetDeviceMemoryUsage()<<'\n';
        CHECK(BbStats::vk_image_bytes.load()==(enabled?0:Count*(1u<<20)));
        for (unsigned i=0;i<Count;++i) {
            const u32 expected=enabled ? 0xff000000u+i*0x070d13u : 0xa5a5a5a5u;
            for (u64 offset=0;offset<(1u<<20);offset+=4) {
                u32 value; std::memcpy(&value,reinterpret_cast<void*>(Guest+(u64(i)<<20)+offset),4); CHECK(value==expected);
            }
        }
        std::sort(timings.begin(),timings.end());
        std::printf("PASS: tiled GC %s, %llu images evicted, %.3f ms p95, %.3f ms worst, %llu bytes safely written\n",
            enabled?"on":"off",static_cast<unsigned long long>(cache.gc_total_evictions),timings[15],timings.back(),
            static_cast<unsigned long long>(cache.gc_tiled_bytes));
    }
};
}
struct Caches {
    VideoCore::PageManager pages{nullptr}; VideoCore::BufferCache buffers; VideoCore::TextureCache textures;
    Caches(Vulkan::Instance& instance,Vulkan::Scheduler& scheduler,Vulkan::Runtime& runtime)
        : buffers{instance,scheduler,runtime,nullptr,textures,pages},textures{instance,scheduler,runtime,nullptr,buffers,pages} {}
};
int main(int argc,char** argv) {
    CHECK(argc>=2);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    _putenv_s("BB_GUEST_IN_PLACE","0"); _putenv_s("BB_WRITE_TRACKING","1"); _putenv_s("BB_GC_BUDGET_MB","1");
    const bool enabled=!std::strcmp(argv[1],"on");
    _putenv_s("BB_IMAGE_WRITEBACK_GC",!std::strcmp(argv[1],"default")?"":enabled?"1":"0");
    CHECK(VirtualAlloc(reinterpret_cast<void*>(Guest),GuestSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)==reinterpret_cast<void*>(Guest));
    Vulkan::Instance instance{-1,false}; Vulkan::Scheduler scheduler{instance,true}; Vulkan::Runtime runtime{instance,scheduler};
    {
        Caches caches{instance,scheduler,runtime};
        if (!std::strcmp(argv[1],"safety")) VideoCore::TiledWritebackTest::Safety(caches.textures);
        else { CHECK(argc==3); VideoCore::TiledWritebackTest::Collect(caches.textures,enabled,argv[2]); }
        scheduler.Finish();
    }
    CHECK(VirtualFree(reinterpret_cast<void*>(Guest),0,MEM_RELEASE));
}
