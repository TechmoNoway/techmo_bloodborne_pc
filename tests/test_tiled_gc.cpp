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
#include <thread>
#include <unordered_map>
#include "bbport_toggles.h"
#include "bbport_hash_reuse.h"
#include "core/memory.h"
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
void PageManager::NoteImageFault(VAddr address,u64 size,u32,u32,u32,u32) {
    CHECK(address>=Guest && address+size<=Guest+GuestSize); // Synthetic CPU invalidation.
}
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
    static void Recycle(TextureCache& cache) {
        const auto retire = [&] {
            cache.scheduler.Finish();
            // Finish waits for GPU completion; CPU retirement is polled separately.
            for(unsigned i=0;i<32;++i) cache.scheduler.PopPendingOperations();
        };
        const bool enabled=ImageRecycler::ConfiguredLimit()!=0;
        cache.image_recycler.Budget(0, 1ull<<30, true);
        const auto first = Make(cache,Guest,128,0xff123456);
        auto& old = cache.slot_images[first];
        const auto handle = old.GetImage();
        old.flags &= ~ImageFlagBits::GpuModified;
        // Queue a use at the current tick: retirement may not expose it early.
        Buffer output(cache.instance,0,old.info.guest_size,MemoryType::HostCached);
        const vk::BufferImageCopy copy{.bufferRowLength=128,.bufferImageHeight=128,
            .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={128,128,1}};
        cache.runtime.DownloadImage(&old,&output,std::span{&copy,1});
        cache.FreeImage(first);
        CHECK(cache.image_recycler.Bytes()==0);
        retire();
        CHECK((cache.image_recycler.Bytes()>0)==enabled);
        const auto hits=BbStats::image_recycle_hits.load();
        const auto second=Make(cache,Guest+(1u<<20),128,0xffabcdef);
        auto& image=cache.slot_images[second];
        CHECK((!enabled || image.GetImage()==handle) && BbStats::image_recycle_hits.load()==hits+unsigned(enabled));
        CHECK(cache.image_recycler.Bytes()==0 && image.uploaded_mips==0);
        cache.runtime.DownloadImage(&image,&output,std::span{&copy,1});retire();
        output.Invalidate(0,image.info.guest_size);
        for(u32 i=0;i<128*128;++i) { u32 value;std::memcpy(&value,output.mapped_data.data()+i*4,4);CHECK(value==0xffabcdef); }
        if (!enabled) {
            cache.FreeImage(second);retire();CHECK(cache.image_recycler.Bytes()==0);
            std::puts("PASS: disabled image retention keeps fresh uploads/readbacks and returns retired allocations");
            return;
        }
        // Exact shape/usage matching; the old live allocation cannot masquerade as a new size.
        image.flags &= ~ImageFlagBits::GpuModified;
        cache.FreeImage(second);retire();
        const auto third=Make(cache,Guest+(2u<<20),256,0xff556677);
        CHECK(cache.slot_images[third].GetImage()!=handle && cache.image_recycler.Bytes()>0);
        cache.FreeImage(third);retire(); // GPU-modified surface is excluded.
        const auto before=cache.image_recycler.Bytes(); CHECK(before>0 && before<=(4u<<20));
        cache.image_recycler.Budget(0,1ull<<30,true,ImageRecycler::Clock::now()+std::chrono::seconds(2));
        CHECK(cache.image_recycler.Bytes()==0);
        const auto fourth=Make(cache,Guest+(3u<<20),128,0xffaabbcc);
        cache.slot_images[fourth].flags &= ~ImageFlagBits::GpuModified;
        cache.FreeImage(fourth);retire();CHECK(cache.image_recycler.Bytes()>0);
        cache.image_recycler.Budget(800u<<20,1ull<<30,true);CHECK(cache.image_recycler.Bytes()==0);
        const auto fifth=Make(cache,Guest+(4u<<20),128,0xffaabbcc);
        cache.slot_images[fifth].flags &= ~ImageFlagBits::GpuModified;
        cache.FreeImage(fifth);retire();CHECK(cache.image_recycler.Bytes()==0);
        std::puts("PASS: retired-only exact image reuse, fresh pixels/identity, incompatible-size rejection, TTL and pressure drain");
    }
    static void Refresh(TextureCache& cache,u32 width=128) {
        const auto id=Make(cache,Guest,width,0xff112233,2);
        auto& image=cache.slot_images[id];
        const bool keep=!(std::getenv("BB_KEEP_MIP_HASHES") && std::getenv("BB_KEEP_MIP_HASHES")[0]=='0');
        const auto fill=[&](u64 offset,u64 size,u32 color) {
            for(u64 i=offset;i<offset+size;i+=4) *reinterpret_cast<u32*>(Guest+i)=color;
            cache.buffer_cache.InvalidateMemory(Guest+offset,size);
            cache.InvalidateMemory(Guest+offset,size);
        };
        unsigned read_pass=0;
        const auto read=[&](u32 first,u32 second) {
            ++read_pass;
            Buffer output(cache.instance,0,image.info.guest_size,MemoryType::HostCached);
            std::array<vk::BufferImageCopy,2> copies{};
            for(u32 m=0;m<2;++m) {
                const auto& mip=image.info.mips_layout[m];
                copies[m]={.bufferOffset=mip.offset,.bufferRowLength=mip.pitch,.bufferImageHeight=mip.height,
                    .imageSubresource={vk::ImageAspectFlagBits::eColor,m,0,1},.imageExtent={width>>m,width>>m,1}};
            }
            cache.runtime.DownloadImage(&image,&output,copies);
            cache.scheduler.Finish();output.Invalidate(0,image.info.guest_size);
            for(u32 m=0;m<2;++m) {
                const auto& mip=image.info.mips_layout[m];
                for(u32 y=0;y<(width>>m);++y) for(u32 x=0;x<(width>>m);++x) {
                    u32 actual;std::memcpy(&actual,output.mapped_data.data()+mip.offset+4*(y*mip.pitch+x),4);
                    if(actual!=(m?second:first)) std::fprintf(stderr,"Refresh read %u mip %u (%u,%u): %08x expected %08x\n",read_pass,m,x,y,actual,m?second:first);
                    CHECK(actual==(m?second:first));
                }
            }
        };
        image.flags &= ~ImageFlagBits::GpuModified;
        fill(0,image.info.guest_size,0xff112233);
        cache.UpdateImage(id);read(0xff112233,0xff112233);
        auto before=BbStats::image_upload_bytes.load();
        cache.InvalidateMemory(Guest,image.info.guest_size);
        cache.UpdateImage(id);
        CHECK(BbStats::image_upload_bytes.load()-before==(keep?0:image.info.guest_size));
        const auto mip0=image.info.mips_layout[0].size;
        before=BbStats::image_upload_bytes.load();
        fill(0,mip0,0xff445566);cache.UpdateImage(id);
        CHECK(BbStats::image_upload_bytes.load()-before==(keep?mip0:image.info.guest_size));
        read(0xff445566,0xff112233);
        // A GPU-only update must survive a CPU dirty notification with unchanged backing bytes.
        image.flags |= ImageFlagBits::GpuModified;
        vk::ClearValue clear{};clear.color.float32=std::array{1.f,0.f,0.f,1.f};
        cache.runtime.ClearImage(&image,{.extent=image.info.resources},clear);cache.scheduler.Finish();
        cache.InvalidateMemory(Guest,image.info.guest_size);
        before=BbStats::image_upload_bytes.load();cache.UpdateImage(id);
        CHECK(BbStats::image_upload_bytes.load()==before);read(0xff0000ff,0xff0000ff);
        // GPU-dirty content is always refreshed even if the CPU hash has not changed.
        cache.InvalidateMemoryFromGPU(Guest,image.info.guest_size);
        cache.buffer_cache.InvalidateMemory(Guest,image.info.guest_size);
        before=BbStats::image_upload_bytes.load();cache.UpdateImage(id);
        CHECK(BbStats::image_upload_bytes.load()-before==image.info.guest_size);
        read(0xff445566,0xff112233);
        cache.FreeImage(id);cache.scheduler.Finish();
        std::puts("PASS: actual cache skips unchanged CPU mips, uploads changed mip, retains GPU-only writes and forces GPU-dirty refresh");
    }
    static void GpuSync(TextureCache& cache) {
        const auto id=Make(cache,Guest,128,0xff112233);
        auto& image=cache.slot_images[id];
        for(u64 i=0;i<image.info.guest_size;i+=4) *reinterpret_cast<u32*>(Guest+i)=0xff112233;
        image.flags &= ~ImageFlagBits::GpuModified;
        cache.buffer_cache.InvalidateMemory(Guest,image.info.guest_size);
        cache.InvalidateMemory(Guest,image.info.guest_size); cache.UpdateImage(id);
        const auto read=[&](u32 expected) {
            Buffer output(cache.instance,0,image.info.guest_size,MemoryType::HostCached);
            const vk::BufferImageCopy copy{.bufferRowLength=128,.bufferImageHeight=128,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={128,128,1}};
            cache.runtime.DownloadImage(&image,&output,std::span{&copy,1});
            cache.scheduler.Finish();output.Invalidate(0,image.info.guest_size);
            for(u32 i=0;i<128*128;++i) { u32 value;std::memcpy(&value,output.mapped_data.data()+i*4,4);CHECK(value==expected); }
        };
        const auto produce=[&](u32 color) {
            auto [buffer,offset]=cache.buffer_cache.ObtainBuffer(Guest,image.info.guest_size,true);
            cache.runtime.FillBuffer(buffer,offset,image.info.guest_size,color);
            cache.InvalidateMemoryFromGPU(Guest+4,4); // Interior write/alias notification.
            CHECK(True(image.flags&ImageFlagBits::GpuDirty));
        };
        const bool reuse=!(std::getenv("BB_REUSE_GPU_SOURCE_HASH") && std::getenv("BB_REUSE_GPU_SOURCE_HASH")[0]=='0');
        const auto hashed=BbStats::image_hash_bytes.load();
        const auto uploaded=BbStats::image_upload_bytes.load();
        produce(0xff445566);cache.UpdateImage(id);read(0xff445566);
        CHECK(BbStats::image_hash_bytes.load()-hashed==(reuse?0:image.info.guest_size));
        CHECK(BbStats::image_upload_bytes.load()-uploaded==image.info.guest_size);
        CHECK(*reinterpret_cast<u32*>(Guest)==0xff112233); // GPU, not stale guest bytes.
        CHECK(!BbStats::enabled || image.hash_reuse_last_blockers==(reuse?0u:1u<<BbHashReuse::Disabled));
        // CPU hash match cannot suppress a pending GPU update.
        cache.MarkAsMaybeDirty(id,image);produce(0xff778899);
        cache.UpdateImage(id);read(0xff778899);
        CHECK(False(image.flags&ImageFlagBits::Dirty));
        CHECK(!BbStats::enabled || (image.hash_reuse_last_blockers&(1u<<BbHashReuse::CpuDirty)));
        // A host backing write bypasses page protection: even an unrelated write
        // conservatively invalidates the reusable CPU reference.
        const u32 host=0xffabcdef;
        CHECK(Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(Guest+(16u<<20)),&host,4));
        auto before=BbStats::image_hash_bytes.load();produce(0xffaabbcc);cache.UpdateImage(id);read(0xffaabbcc);
        CHECK(BbStats::image_hash_bytes.load()-before==image.info.guest_size);
        CHECK(!BbStats::enabled || (image.hash_reuse_last_blockers&(1u<<BbHashReuse::HostWriteChanged)));
        __atomic_add_fetch(&epoch,2,__ATOMIC_RELEASE);
        before=BbStats::image_hash_bytes.load();produce(0xffddeeff);cache.UpdateImage(id);read(0xffddeeff);
        CHECK(BbStats::image_hash_bytes.load()-before==image.info.guest_size);
        CHECK(!BbStats::enabled || (image.hash_reuse_last_blockers&(1u<<BbHashReuse::MappingChanged)));
        // Unwatched guest modes never reuse a reference.
        Detail::write_tracking.store(2);
        before=BbStats::image_hash_bytes.load();produce(0xff102030);cache.UpdateImage(id);read(0xff102030);
        CHECK(BbStats::image_hash_bytes.load()-before==image.info.guest_size);
        CHECK(!BbStats::enabled || (image.hash_reuse_last_blockers&(1u<<BbHashReuse::TrackingOff)));
        Detail::write_tracking.store(1);
        cache.UntrackImage(id);cache.TrackImage(id);
        before=BbStats::image_hash_bytes.load();produce(0xff405060);cache.UpdateImage(id);read(0xff405060);
        CHECK(BbStats::image_hash_bytes.load()-before==image.info.guest_size);
        CHECK(!BbStats::enabled || (image.hash_reuse_last_blockers&(1u<<BbHashReuse::TrackingGap)));
        if(BbStats::enabled) BbHashReuse::Print();
        // A neighbour within the page registry does not invalidate this resource.
        cache.InvalidateMemoryFromGPU(Guest+image.info.guest_size,4);
        CHECK(False(image.flags&ImageFlagBits::GpuDirty));
        cache.InvalidateMemoryFromGPU(Guest,0);
        cache.FreeImage(id);cache.scheduler.Finish();
        std::puts("PASS: real GPU buffer -> tiled image, interior invalidation, mixed CPU/GPU dirty, required copy with CPU hash reuse, host writes/mappings/unwatched fallbacks");
    }
    static void Lifetime(TextureCache& cache) {
        const auto id=Make(cache,Guest,128,0xff112233);
        auto& image=cache.slot_images[id];
        // Guest backing agrees with this now-clean image.
        for (u64 i=0;i<image.info.guest_size;i+=4)
            *reinterpret_cast<u32*>(Guest+i)=0xff112233;
        image.flags &= ~ImageFlagBits::GpuModified;
        const auto collect=[&] {
            cache.gc_tick+=1000;cache.gc_tick_at_second.fill(cache.gc_tick-100);
            cache.memory_sample_time={};cache.GarbageCollectImages();
        };
        cache.MarkFound(id); // Cached lookup does not touch the LRU list.
        collect();CHECK(cache.gc_total_evictions==0); // Current/uncompleted use.
        cache.scheduler.Record([](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier2({.memoryBarrierCount=0});
        });
        cache.scheduler.Finish();
        CHECK(cache.scheduler.IsFree(image.LastUse()));
        collect();CHECK(cache.gc_total_evictions==0); // Completed but recently used.
        __atomic_store_n(&image.last_use_ms,u64{1},__ATOMIC_RELAXED); // Synthetic aging.
        collect();CHECK(cache.gc_total_evictions==1); // Actually cold and completed.
        cache.scheduler.Finish();
        std::puts("PASS: cached texture use survives GC before completion and during wall-time grace; cold completed texture is collected");
    }
    static void Metadata(TextureCache& cache) {
        constexpr VAddr meta_address=Guest+(4<<20);constexpr u32 bytes=256;
        auto first=Make(cache,Guest,64,0xff112233);
        auto second=Make(cache,Guest+(1<<20),64,0xff445566);
        TextureCache::ImageDesc desc{};desc.info=cache.slot_images[first].info;
        desc.info.meta_info.cmask_addr=meta_address;
        (void)cache.FindRenderTarget(first,desc);CHECK(cache.GetMetaInfo(meta_address)->owner_id==first);
        CHECK(cache.TouchMeta(meta_address,0,false));CHECK(!cache.IsMetaCleared(meta_address,0));
        (void)cache.FindRenderTarget(first,desc);CHECK(!cache.IsMetaCleared(meta_address,0));
        CHECK(!cache.TouchMeta(meta_address,32,true));
        std::memset(reinterpret_cast<void*>(meta_address),0x5a,bytes);
        const auto [arena,offset]=cache.buffer_cache.ObtainBuffer(meta_address,bytes,false,true);
        Buffer readback{cache.instance,0,bytes,MemoryType::HostCached};
        const auto check=[&](u32 expected) {
            const vk::BufferCopy copy{offset,0,bytes};cache.runtime.CopyBuffer(arena,&readback,std::span{&copy,1});
            cache.scheduler.Finish();readback.Invalidate(0,bytes);
            for(u32 i=0;i<bytes;i+=4) { u32 value;std::memcpy(&value,readback.mapped_data.data()+i,4);
                if(value!=expected) std::fprintf(stderr,"CMask byte %u: actual %08x expected %08x\n",i,value,expected);
                CHECK(value==expected); }
            for(u32 i=0;i<bytes;++i) CHECK(reinterpret_cast<const u8*>(meta_address)[i]==0x5a);
        };
        check(0xffffffff);CHECK(cache.buffer_cache.IsRegionGpuModified(meta_address,bytes));
        for(auto [samples,pattern]:{std::pair{1u,0xffffffffu},std::pair{2u,0xddddddddu},std::pair{4u,0xeeeeeeeeu},std::pair{8u,0xffffffffu}}) {
            auto meta=*cache.GetMetaInfo(meta_address);meta.num_samples=samples;
            cache.RegisterMeta(meta_address,meta);
            CHECK(cache.buffer_cache.SynchronizeMemoryFromImage(arena,meta_address,bytes));check(pattern);
        }
        for(u32 samples:{0u,3u,16u}) {
            auto meta=*cache.GetMetaInfo(meta_address);meta.num_samples=samples;cache.RegisterMeta(meta_address,meta);
            cache.runtime.FillBuffer(arena,offset,bytes,0x12345678);
            CHECK(!cache.buffer_cache.SynchronizeMemoryFromImage(arena,meta_address,bytes));check(0x12345678);
        }
        auto meta=*cache.GetMetaInfo(meta_address);meta.num_samples=1;cache.RegisterMeta(meta_address,meta);
        CHECK(!cache.buffer_cache.SynchronizeMemoryFromImage(arena,meta_address,3));
        desc.info=cache.slot_images[second].info;desc.info.meta_info.cmask_addr=meta_address;
        (void)cache.FindRenderTarget(second,desc);meta=*cache.GetMetaInfo(meta_address);
        CHECK(cache.IsMetaCleared(meta_address,0));cache.FreeImage(first);
        CHECK(cache.GetMetaInfo(meta_address)->owner_id==second);
        auto stale=meta;cache.FreeImage(second);CHECK(!cache.GetMetaInfo(meta_address));
        cache.scheduler.Finish();cache.scheduler.PopPendingOperations();
        CHECK(!cache.slot_images.is_allocated(first) && !cache.slot_images.is_allocated(second));
        const auto third=Make(cache,Guest+(2<<20),64,0xff778899);
        CHECK(third==first || third==second);
        cache.surface_metas[meta_address]=stale;CHECK(!cache.GetMetaInfo(meta_address));
        CHECK(!cache.ClearMeta(meta_address));CHECK(!cache.IsMetaCleared(meta_address,0));
        meta.owner_id=third;meta.owner_uid=cache.slot_images[third].image_uid;cache.RegisterMeta(meta_address,meta);
        cache.slot_images[third].info.meta_info.cmask_addr=meta_address;
        desc.info=cache.slot_images[third].info;desc.info.meta_info.cmask_addr=meta_address+256;
        (void)cache.FindRenderTarget(third,desc);CHECK(!cache.GetMetaInfo(meta_address));CHECK(cache.GetMetaInfo(meta_address+256));
        desc.info.meta_info.cmask_addr=0;(void)cache.FindRenderTarget(third,desc);CHECK(!cache.GetMetaInfo(meta_address+256));
        cache.FreeImage(third);cache.scheduler.Finish();
        puts("PASS: real CMask fills/readbacks at 1/2/4/8 samples, unsupported/alignment guards, GPU ownership, preserved clear state and owner/address/slot reuse");
    }
    static void Samplers(TextureCache& cache) {
        AmdGpu::Sampler sampler{};sampler.clamp_x.Assign(AmdGpu::ClampMode::ClampBorder);sampler.clamp_y.Assign(AmdGpu::ClampMode::ClampBorder);
        sampler.border_color_type.Assign(AmdGpu::BorderColor::Custom);
        constexpr VAddr colors=Guest+(20<<20);
        AmdGpu::BorderColorBuffer border{};border.base_addr=colors>>8;
        std::array<float,4> red{1,0,0,1},green{0,1,0,1};std::memcpy(reinterpret_cast<void*>(colors),red.data(),16);
        const auto first=cache.GetSampler(sampler,border,false,0);
        auto hits=cache.sampler_memo_hits;CHECK(cache.GetSampler(sampler,border,false,0)==first);CHECK(cache.sampler_memo_hits==hits+1);
        const auto key=MakeSamplerKey(cache.instance,sampler,border,false,0);
        std::memcpy(reinterpret_cast<void*>(colors),green.data(),16);
        const auto changed=cache.GetSampler(sampler,border,false,0);
        if(cache.instance.IsCustomBorderColorSupported()) CHECK(changed!=first);
        std::memcpy(reinterpret_cast<void*>(colors+256),green.data(),16);auto alternate=border;alternate.base_addr=(colors+256)>>8;
        CHECK(cache.GetSampler(sampler,alternate,false,0)==changed);
        CHECK(cache.GetSampler(sampler,border,true,0));CHECK(cache.GetSampler(sampler,border,false,0.5f));
        struct Collision { size_t operator()(const SamplerKey&)const { return 0; } };
        std::unordered_map<SamplerKey,u32,Collision> collision;
        auto other=key;other.border[0]^=1;collision[key]=1;collision[other]=2;CHECK(collision.size()==2 && collision.at(key)==1);
        std::vector<std::thread> workers;
        for(u32 t=0;t<4;++t) workers.emplace_back([&,t] {
            for(u32 i=0;i<32;++i) {
                CHECK(cache.GetSampler(sampler,border,false,float(1+t*32+i)/32));
                CHECK(cache.GetSampler(sampler,border,false,0)==changed);
            }
        });
        for(auto& worker:workers) worker.join();
        const auto current=MakeSamplerKey(cache.instance,sampler,border,false,0);
        CHECK(cache.samplers.at(current).last_gpu_tick==cache.scheduler.CurrentTick());
        cache.trigger_gc_samplers=0;cache.pressure_gc_samplers=cache.critical_gc_samplers=~u64{0};cache.gc_tick=1000;
        const auto count=cache.samplers.size();cache.GarbageCollectSamplers();CHECK(cache.samplers.size()==count);
        Buffer probe{cache.instance,0,16,MemoryType::DeviceLocal};cache.runtime.FillBuffer(&probe,0,16,0);cache.scheduler.Finish();
        CHECK(cache.GetSampler(sampler,border,false,0));hits=cache.sampler_memo_hits;
        CHECK(cache.GetSampler(sampler,border,false,0));CHECK(cache.sampler_memo_hits==hits+1);
        CHECK(cache.samplers.at(current).last_gpu_tick==cache.scheduler.CurrentTick());
        cache.runtime.FillBuffer(&probe,0,16,0);cache.scheduler.Finish();
        for(u32 pass=0;!cache.samplers.empty() && pass<64;++pass) {
            cache.gc_tick+=100;const auto before=cache.samplers.size();cache.GarbageCollectSamplers();
            CHECK(before-cache.samplers.size()<=10);
        }
        CHECK(cache.samplers.empty());const auto misses=cache.sampler_memo_misses;
        CHECK(cache.GetSampler(sampler,border,false,0));CHECK(cache.sampler_memo_misses==misses+1);
        cache.runtime.FillBuffer(&probe,0,16,0);cache.scheduler.Finish();
        puts("PASS: full sampler keys/custom border updates, collision equality, concurrent memo/rehash, GPU retirement, bounded GC and stale-memo rejection");
    }
    static ImageId Make(TextureCache& cache,VAddr address,u32 width,u32 color,u32 levels=1) {
        ImageInfo info{}; info.guest_address=address; info.type=AmdGpu::ImageType::Color2D;
        info.pixel_format=vk::Format::eR8G8B8A8Unorm; info.size={width,width,1};
        info.pitch=width; info.num_bits=32; info.resources={levels,1}; info.props.is_tiled=1;
        info.tile_mode=AmdGpu::TileMode::Thin1DThin;
        info.array_mode=AmdGpu::ArrayMode::Array1DTiledThin1; info.UpdateSize();
        const auto id=cache.slot_images.insert(cache.instance,cache.runtime,cache.slot_image_views,info,&cache.image_recycler);
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
        scheduler.SetSubmitCallback([&](Vulkan::SubmitInfo& info) { runtime.FlushBarriers();caches.buffers.SubmitPendingArenaBinds(info); });
        if (!std::strcmp(argv[1],"metadata")) VideoCore::TiledWritebackTest::Metadata(caches.textures);
        else if (!std::strcmp(argv[1],"samplers")) VideoCore::TiledWritebackTest::Samplers(caches.textures);
        else if (!std::strcmp(argv[1],"gpu-sync")) VideoCore::TiledWritebackTest::GpuSync(caches.textures);
        else if (!std::strcmp(argv[1],"recycle")) VideoCore::TiledWritebackTest::Recycle(caches.textures);
        else if (!std::strcmp(argv[1],"refresh-large")) VideoCore::TiledWritebackTest::Refresh(caches.textures,1024);
        else if (!std::strcmp(argv[1],"refresh")) VideoCore::TiledWritebackTest::Refresh(caches.textures);
        else if (!std::strcmp(argv[1],"lifetime")) VideoCore::TiledWritebackTest::Lifetime(caches.textures);
        else if (!std::strcmp(argv[1],"safety")) VideoCore::TiledWritebackTest::Safety(caches.textures);
        else { CHECK(argc==3); VideoCore::TiledWritebackTest::Collect(caches.textures,enabled,argv[2]); }
        scheduler.Finish();
        scheduler.SetSubmitCallback({});
    }
    CHECK(VirtualFree(reinterpret_cast<void*>(Guest),0,MEM_RELEASE));
}
