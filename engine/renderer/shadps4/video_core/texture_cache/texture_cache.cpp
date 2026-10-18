// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>
#include <algorithm>

#include "bbport_toggles.h"
#include "bbport_free_check.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/hash.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/gc_policy.h"
#include <limits>
#include "video_core/texture_cache/overlap_diagnostics.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/image_lifetime.h"
#include "bbport_image_hash.h"
#include "bbport_hash_reuse.h"
#include "video_core/texture_cache/tile_manager.h"

namespace VideoCore {

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

namespace {
using Binding = TextureCache::BindingType;

void TraceOverlap(const ImageInfo& requested, Binding binding, ImageId id, const Image* candidate,
                  std::string_view action, std::string_view reason, int mip = -1, int slice = -1) {
    if (!ImageOverlapLogging()) {
        return;
    }

    // Detailed Logs is meant for short diagnostic captures. Skip the extremely common
    // exact-reuse path and rate-limit the rest so a loading screen cannot create a multi-GB log.
    if (action == "reuse") {
        return;
    }
    static std::atomic<u64> emitted{0};
    const u64 index = emitted.fetch_add(1, std::memory_order_relaxed);
    if (index >= 512 && (index % 1024) != 0) {
        return;
    }

    const ImageInfo empty{};
    const auto& cached = candidate ? candidate->info : empty;
    const auto requested_format = vk::to_string(requested.pixel_format);
    const auto cached_format = vk::to_string(cached.pixel_format);
    std::printf(
        "TextureDiag overlap[%llu]: action=%.*s reason=%.*s req_addr=0x%llx req_size=%llu "
        "req_fmt=%s req=%ux%ux%u mips=%u layers=%u type=%llu binding=%u "
        "candidate=%u cand_addr=0x%llx cand_size=%llu cand_fmt=%s cand=%ux%ux%u "
        "cand_mips=%u cand_layers=%u cand_type=%llu bound=%u target=%u view_mip=%d "
        "view_slice=%d\n",
        static_cast<unsigned long long>(index), static_cast<int>(action.size()), action.data(),
        static_cast<int>(reason.size()), reason.data(),
        static_cast<unsigned long long>(requested.guest_address),
        static_cast<unsigned long long>(requested.guest_size), requested_format.c_str(),
        requested.size.width, requested.size.height, requested.size.depth,
        requested.resources.levels, requested.resources.layers,
        static_cast<unsigned long long>(u64(requested.type)), u32(binding), id.index,
        static_cast<unsigned long long>(cached.guest_address),
        static_cast<unsigned long long>(cached.guest_size), cached_format.c_str(),
        cached.size.width, cached.size.height, cached.size.depth, cached.resources.levels,
        cached.resources.layers, static_cast<unsigned long long>(u64(cached.type)),
        candidate ? unsigned(candidate->binding.is_bound) : 0u,
        candidate ? unsigned(candidate->binding.is_target) : 0u, mip, slice);
    std::fflush(stdout);
}

bool ReusableFormat(const ImageInfo& requested, const ImageInfo& cached, Binding binding,
                    bool exact) {
    if (cached.pixel_format == vk::Format::eUndefined ||
        (exact && requested.pixel_format != cached.pixel_format)) {
        return false;
    }
    if (binding == Binding::DepthTarget) {
        return cached.props.is_depth && requested.props.has_stencil == cached.props.has_stencil &&
               requested.num_bits == cached.num_bits;
    }
    if (binding == Binding::Storage || binding == Binding::RenderTarget) {
        if (cached.props.is_depth) {
            return false;
        }
    }
    if (binding == Binding::Texture && cached.props.is_depth &&
        Vulkan::LiverpoolToVK::IsFormatDepthCompatible(requested.pixel_format)) {
        // ImageView promotes R16/R32 sampling to the cached depth image's actual format.
        return true;
    }
    return IsVulkanFormatCompatible(cached.pixel_format, requested.pixel_format);
}

std::string_view ReuseRejection(const ImageInfo& requested, const ImageInfo& cached, Binding binding,
                               bool exact, int& mip, int& slice) {
    mip = slice = 0;
    if (requested.guest_address != cached.guest_address) {
        mip = requested.MipOf(cached);
        slice = mip >= 0 ? requested.SliceOf(cached, mip) : -1;
        if (mip < 0 || slice < 0) {
            return "not an aligned, contained mip/slice";
        }
    }
    if (const auto reason = requested.ViewRejection(cached, {u32(mip), u32(slice)}); !reason.empty()) {
        return reason;
    }
    return ReusableFormat(requested, cached, binding, exact)
               ? std::string_view{} : "format or binding requires another backing image";
}
} // namespace

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                           BufferCache& buffer_cache_, PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, runtime, buffer_cache.GetStreamBuffer()},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {

    u32 max_samplers = instance.GetMaxSamplerAllocationCount();
    trigger_gc_samplers = max_samplers * 3 / 4;
    pressure_gc_samplers = max_samplers * 7 / 8;
    critical_gc_samplers = max_samplers * 15 / 16;

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    memory_pressure.Update(0, instance.GetTotalMemoryBudget());
    trigger_gc_memory = memory_pressure.marks.trigger;
    pressure_gc_memory = memory_pressure.marks.pressure;
    critical_gc_memory = memory_pressure.marks.critical;
}

TextureCache::~TextureCache() {
    if (memory_log) std::fclose(memory_log);
}

std::string TextureCache::DescribeImagesIn(VAddr addr, u64 size) {
    std::unique_lock lock{mutex, std::try_to_lock}; // the caller may hold it already
    if (!lock.owns_lock()) {
        return " [images: not looked up]";
    }
    std::string out;
    int shown = 0;
    ForEachImageInRegion(addr, size, [&](ImageId, Image& image) {
        if (shown++ < 3) {
            out += fmt::format(" [image {:#x}+{:#x} {}x{} fmt {} tile {}{}{}]", image.info.guest_address,
                               image.info.guest_size, image.info.size.width,
                               image.info.size.height, u32(image.info.pixel_format),
                               u32(image.info.tile_mode), image.usage.render_target ? " RT" : "",
                               True(image.flags & ImageFlagBits::GpuModified) ? " gpu" : "");
        }
    });
    if (shown > 3) {
        out += fmt::format(" +{} more", shown - 3);
    }
    return out;
}

void TextureCache::ProcessDownloadImages() {
    // bbport: called for every fence (~450 a frame): the lock only when something is queued.
    if (!downloads_queued.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lk{download_images_mutex};
    downloads_queued.store(false, std::memory_order_relaxed);
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id, true);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id, bool sync) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    const u32 download_size = image.info.pitch * image.info.size.height * image.info.size.depth *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    vk::BufferImageCopy image_copy = {
        .bufferOffset = 0,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    // BB_GUEST_IN_PLACE: publish the image to the buffer backing in GPU command order,
    // including when that backing is a VRAM copy. Waiting for the CPU readback below
    // leaves subsequent buffer reads using an older frame (upstream issue #122).
    if (GuestInPlace() && image.info.guest_address != 0) {
        const auto [arena, offset] =
            buffer_cache.ObtainBuffer(image.info.guest_address, download_size, true);
        image_copy.bufferOffset = offset;
        runtime.DownloadImage(&image, arena, std::span{&image_copy, 1});
        if (buffer_cache.IsInPlace(image.info.guest_address, download_size)) {
            return;
        }
        // A VRAM copy still needs the existing guest RAM readback for CPU consumers.
    }
    const auto download =
        runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16, !sync);
    const vk::BufferImageCopy image_download = {
        .bufferOffset = download.offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
    if (sync) {
        scheduler.Finish();
        download.Invalidate();
        BbFreeCheck::Check(image.info.guest_address, download_size, download.mapped,
                           BbFreeCheck::ImageDownload);
        Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(image.info.guest_address),
                                                  download.mapped, download_size);
        if (!WriteTracking()) {
            buffer_cache.InvalidateMemory(image.info.guest_address, download_size);
        }
    } else {
        scheduler.DeferPriorityOperation(
            [this, device_addr = image.info.guest_address, download, download_size] {
                download.Invalidate();
                BbFreeCheck::Check(device_addr, download_size, download.mapped,
                                   BbFreeCheck::ImageDownload);
                Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr),
                                                          download.mapped, download_size);
                if (!WriteTracking()) {
                    buffer_cache.InvalidateMemory(device_addr, download_size);
                }
                runtime.GetStagingPool().FreeDeferred(download);
            });
    }
}

bool TextureCache::WriteBackTiledImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return true; // guest memory already holds its contents
    }
    const auto& info = image.info;
    // Experimental path: tracked CPU writes and completed GPU use are mandatory.
    if (!WriteTracking() || !image.IsTracked() || !image.LastUse() ||
        image.LastUse() >= scheduler.CurrentTick() ||
        !scheduler.IsFree(image.LastUse()) || info.guest_size > (16ULL << 20)) return false;
    if (!info.props.is_tiled || info.guest_address == 0 || info.guest_size == 0 ||
        !image.backing || image.backing->image.image_ci.format != info.pixel_format) {
        return false;
    }
    // Only the plain colour surface is rebuilt: compression metadata, depth/stencil, MSAA,
    // block-compressed, volume and thick-tiled layouts stay resident. So does an image whose
    // newest contents may be in a reduced-resolution scene proxy (SceneTargets).
    const auto& meta = info.meta_info;
    if (meta.cmask_addr || meta.fmask_addr || meta.htile_addr || info.stencil_addr ||
        info.props.is_depth || info.props.has_stencil || info.props.is_block ||
        info.props.is_volume || info.num_samples != 1 || image.scene_proxy ||
        image.aspect_mask != vk::ImageAspectFlags{vk::ImageAspectFlagBits::eColor} ||
        AmdGpu::GetMicroTileThickness(info.array_mode) != 1) {
        return false;
    }
    switch (info.num_bits) {
    case 8:
    case 16:
    case 32:
    case 64:
    case 128:
        break;
    default:
        return false; // no tiling shader for it
    }
    const u32 bytes_per_texel = info.num_bits / 8;
    if (info.tile_mode != AmdGpu::TileMode::Thin1DThin &&
        info.tile_mode != AmdGpu::TileMode::Thin2DThin &&
        info.tile_mode != AmdGpu::TileMode::Display1DThin &&
        info.tile_mode != AmdGpu::TileMode::Display2DThin) {
        return false; // other mode variants have no writeback regression coverage
    }
    // The tiling shader runs whole groups of 64 texels over guest_size.
    if (info.guest_size % (bytes_per_texel * 64) != 0 ||
        info.guest_size > instance.MaxStorageBufferRange() ||
        info.guest_size / (bytes_per_texel * 64) > instance.MaxComputeWorkGroupCountX()) {
        return false;
    }
    const u32 levels = info.resources.levels;
    const u32 layers = info.resources.layers;
    if (levels > info.mips_layout.size() || layers == 0 ||
        !GcPolicy::WritebackMipChain(AmdGpu::IsMacroTiled(info.array_mode), levels)) {
        return false;
    }
    // The shader numbers texels mip after mip (x, y, then slice); the copies place mip m at its
    // offset with rows of `pitch` texels and slices of `height` rows. Both agree, and every byte
    // of guest_size is produced, only when the mips are contiguous and each is exactly
    // pitch * height * layers texels.
    u64 expected_offset = 0;
    for (u32 mip = 0; mip < levels; ++mip) {
        const auto& mip_info = info.mips_layout[mip];
        if (mip_info.pitch == 0 || mip_info.height == 0 || mip_info.pitch % 8 != 0 ||
            mip_info.height % 8 != 0 || mip_info.offset != expected_offset ||
            !GcPolicy::FullMip(mip_info.pitch, mip_info.height, layers, bytes_per_texel,
                               mip_info.size) ||
            mip_info.size > info.guest_size - expected_offset) {
            return false;
        }
        expected_offset += mip_info.size;
    }
    if (expected_offset != info.guest_size) {
        return false;
    }
    auto* const memory = Core::Memory::Instance();
    const VAddr address = info.guest_address;
    const u64 size = info.guest_size;
    if (address > std::numeric_limits<VAddr>::max() - size ||
        image.track_addr > address || image.track_addr_end < address + size) return false;
    const auto budget = instance.GetDeviceMemorySnapshot();
    if (!budget.budget || budget.usage >= budget.budget ||
        size > (budget.budget - budget.usage) / 2) return false;
    const u64 generation = memory->MappingGeneration();
    if (generation & 1) return false;
    // A changed mapping generation aborts the final store under the runtime mapping lock.
    if (memory->ClampRangeSize(address, size) != size) {
        return false;
    }
    // The buffer cache may hold GPU writes over the range that guest memory has not seen.
    if (buffer_cache.IsRegionGpuModified(address, size)) {
        return false;
    }
    // Other images over the range: one holding GPU data would be overwritten in guest memory;
    // the clean ones are invalidated once the write-back has landed.
    ImageIds overlapping;
    bool other_gpu_data = false;
    ForEachImageInRegion(address, size, [&](ImageId other_id, Image& other) {
        if (other_id == image_id) {
            return;
        }
        if (True(other.flags & ImageFlagBits::GpuModified)) {
            other_gpu_data = true;
        }
        overlapping.push_back(other_id);
    });
    if (other_gpu_data) {
        return false;
    }

    boost::container::small_vector<vk::BufferImageCopy, 14> copies;
    for (u32 mip = 0; mip < levels; ++mip) {
        const auto& mip_info = info.mips_layout[mip];
        const u32 width = std::max(info.size.width >> mip, 1u);
        const u32 height = std::max(info.size.height >> mip, 1u);
        if (mip_info.pitch < width || mip_info.height < height) return false;
        // The inverse of RefreshImage's upload copies.
        copies.push_back({
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {std::min(mip_info.pitch, width), std::min(mip_info.height, height), 1},
        });
    }

    const auto started = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        // Includes allocation, GPU wait, backing store and temporary-buffer destruction.
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        gc_tiled_ms += elapsed;
        gc_tiled_max_ms = std::max(gc_tiled_max_ms, elapsed);
    };
    ++gc_tiled_attempts;
    // A host-visible copy of the guest surface, tiled over on the GPU, then stored back whole.
    const auto download = runtime.GetStagingPool().Request(
        size, MemoryType::HostCached, std::max<u64>(instance.StorageMinAlignment(), 16));
    if (!download.mapped) {
        return false;
    }
    memory->ReadBacking(address, download.mapped, size);
    download.Flush();
    scheduler.EndRendering();
    const auto linear_storage = tile_manager.TileImageOver(image, copies, download.buffer, download.offset);
    runtime.FlushBarriers();
    // Synchronously, while the image still protects its pages (see GarbageCollectImages).
    scheduler.Finish();
    download.Invalidate();
    BbFreeCheck::Check(address, size, download.mapped, BbFreeCheck::ImageDownload);
    // All-or-nothing: stale readbacks never write into a recycled guest allocation.
    if (!memory->TryWriteBackingUnchanged(address, download.mapped, size, generation)) return false;
    buffer_cache.InvalidateMemory(address, size);
    ++gc_tiled_writebacks;
    gc_tiled_bytes += size;
    // As a CPU write over them would: re-uploaded from guest memory at their next use.
    for (const ImageId other_id : overlapping) {
        Image& other = slot_images[other_id];
        other.flags |= ImageFlagBits::CpuDirty;
        UntrackImage(other_id);
    }
    return true;
}

void TextureCache::DumpImagesAt(VAddr address, const char* dir) {
    boost::container::small_vector<ImageId, 4> ids;
    {
        std::scoped_lock lock{mutex};
        ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
            if (image.info.guest_address == address) {
                ids.push_back(image_id);
            }
        });
    }
    std::printf("Image dump %#llx: %zu images\n", static_cast<unsigned long long>(address),
                ids.size());
    u32 n = 0;
    for (const ImageId image_id : ids) {
        Image& image = slot_images[image_id];
        const auto format = vk::to_string(image.info.pixel_format);
        const u32 width = image.info.size.width, height = image.info.size.height;
        std::printf("  image %u: %s %ux%u tile %u, flags %#x, layers %u, mips %u, %s\n",
                    image_id.index, format.c_str(), width, height,
                    static_cast<u32>(image.info.tile_mode), static_cast<u32>(image.flags),
                    image.info.resources.layers, image.info.resources.levels,
                    image.info.props.is_depth ? "depth" : "color");
        if (image.info.props.is_block) {
            continue;
        }
        const u32 bytes_per_pixel = image.info.props.is_depth ? 4 : image.info.num_bits / 8;
        const u64 size = u64(width) * height * bytes_per_pixel;
        const auto download = runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16);
        const vk::BufferImageCopy copy = {
            .bufferOffset = download.offset,
            .imageSubresource = {image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                           : vk::ImageAspectFlagBits::eColor,
                                 0, 0, 1},
            .imageExtent = {width, height, 1},
        };
        runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
        scheduler.Finish();
        download.Invalidate();
        const std::string path = std::format("{}/img_{:x}_{}_{}x{}_{}.raw", dir, address, n++, width,
                                             height, format);
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(download.mapped, 1, size, f);
            std::fclose(f);
        }
    }
}

/// bbport: the guest memory a MaybeCpuDirty check compares, the same at marking and at
/// refresh (they hashed different ranges, the whole image and its first 8x8 pixels, so the
/// first check never matched). Such an image lies within the faulting page: cheap to hash
/// whole, and a CPU write past its first pixels still counts.
u64 TextureCache::MaybeDirtyHash(const Image& image) {
    return BbImageHash::Serial(std::bit_cast<const u8*>(image.info.guest_address), image.info.guest_size);
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        image.hash = MaybeDirtyHash(image);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            PageManager::NoteImageFault(image.info.guest_address, image.info.guest_size,
                                        image.info.size.width, image.info.size.height,
                                        u32(image.info.pixel_format), u32(image.info.tile_mode));
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    if (!max_size || address > std::numeric_limits<VAddr>::max() - max_size) return;
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId, Image& image) {
        // The page registry is coarser than a resource. Include interior writes
        // and aliases, but exclude neighbours which only share a registry page.
        if (!image.Overlaps(address, max_size)) return;
        image.flags |= ImageFlagBits::GpuDirty;
        if (BbStats::enabled) {
            ++image.gpu_write_count;
            image.gpu_write_address = address;
            image.gpu_write_size = max_size;
        }
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id, bool exact_fmt) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = !requested_info.ViewRejection(cache_image.info).empty() ||
                    (exact_fmt && requested_info.pixel_format != cache_image.info.pixel_format);

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        // Use a real covering allocation's layout. Combining independently larger mip/layer
        // counts or recomputing attachment padding can invent guest memory/shift later mips.
        // The existing color-channel -> MS depth conversion has different samples/bpp but the
        // same byte footprint; normalize only these two fields for the geometry check.
        auto source_layout = cache_image.info;
        source_layout.num_bits = requested_info.num_bits;
        source_layout.num_samples = requested_info.num_samples;
        const bool request_covers = source_layout.ViewRejection(requested_info).empty();
        const bool cache_covers = requested_info.ViewRejection(source_layout).empty();
        if (!request_covers && !cache_covers) {
            TraceOverlap(requested_info, binding, cache_image_id, &cache_image, "reject",
                         "depth conversion has incompatible guest subresource layout");
            return {};
        }
        auto new_info = request_covers ? requested_info : source_layout;
        new_info.pixel_format = requested_info.pixel_format;
        new_info.props.is_depth = requested_info.props.is_depth;
        new_info.props.has_stencil = requested_info.props.has_stencil;
        new_info.meta_info = requested_info.meta_info;
        new_info.stencil_addr = requested_info.stencil_addr;
        new_info.stencil_size = requested_info.stencil_size;
        int mip{}, slice{};
        if (!ReuseRejection(requested_info, new_info, binding, exact_fmt, mip, slice).empty()) {
            return {};
        }
        const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info, &image_recycler);
        RegisterImage(new_image_id);

        // SlotVector insertion may relocate images. Reacquire the source after allocation.
        auto& source_image = slot_images[cache_image_id];
        auto& new_image = slot_images[new_image_id];
        new_image.usage = source_image.usage;
        RefreshImage(new_image);
        RefreshImage(source_image);
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        runtime.CopyColorAndDepth(&source_image, &new_image);

        // Keep bound attachments valid until the renderer notices the replacement.
        if (source_image.binding.is_bound || source_image.binding.is_target) {
            source_image.binding.needs_rebind = 1u;
        }
        new_image.binding.is_target = source_image.binding.is_target;
        FreeImage(cache_image_id);
        TrackImage(new_image_id);
        return new_image_id;
    }

    int mip{}, slice{};
    return ReuseRejection(requested_info, cache_image.info, binding, exact_fmt, mip, slice).empty()
               ? cache_image_id : ImageId{};
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                         BindingType binding,
                                                         ImageId cache_image_id,
                                                         ImageId merged_image_id,
                                                         bool exact_fmt) {
    auto& cached = slot_images[cache_image_id];
    if (cache_image_id == merged_image_id || False(cached.flags & ImageFlagBits::Registered) ||
        cached.info.pixel_format == vk::Format::eUndefined) {
        // Undefined images are stencil/depth association records, not reusable Vulkan images.
        return {{}, -1, -1};
    }
    const auto reject = [&](std::string_view reason) -> std::tuple<ImageId, int, int> {
        TraceOverlap(image_info, binding, cache_image_id, &cached, "reject", reason);
        // Retain the old cheap retirement of cold aliases, but leave bound/GPU-modified
        // images to normal GC (which can write them back). Rejection must not lose GPU data.
        if (!cached.binding.is_bound && !cached.binding.is_target &&
            False(cached.flags & ImageFlagBits::GpuModified) &&
            scheduler.CurrentTick() - cached.LastUse() > NumFramesBeforeRemoval) {
            FreeImage(cache_image_id);
        }
        return {{}, -1, -1};
    };

    // Once a covering image has been selected, only merge its actual child subresources.
    // Do not replace it with a later, smaller alias and lose the previously selected view.
    if (!merged_image_id && image_info.guest_address == cached.info.guest_address) {
        if (image_info.BlockDim() != cached.info.BlockDim() ||
            image_info.num_bits * image_info.num_samples !=
                cached.info.num_bits * cached.info.num_samples) {
            return reject("different block dimensions or sample footprint");
        }
        if (image_info.array_mode != cached.info.array_mode ||
            image_info.tile_mode != cached.info.tile_mode ||
            image_info.alt_tile != cached.info.alt_tile) {
            return reject("different guest tiling layout");
        }
        if (image_info.type != cached.info.type) {
            const bool volume_copy =
                (image_info.type == AmdGpu::ImageType::Color3D &&
                 cached.info.type == AmdGpu::ImageType::Color2D &&
                 image_info.size.depth == cached.info.resources.layers) ||
                (cached.info.type == AmdGpu::ImageType::Color3D &&
                 image_info.type == AmdGpu::ImageType::Color2D &&
                 cached.info.size.depth == image_info.resources.layers);
            if (volume_copy && image_info.guest_size == cached.info.guest_size &&
                image_info.resources.levels == cached.info.resources.levels &&
                image_info.IsCompatible(cached.info)) {
                TraceOverlap(image_info, binding, cache_image_id, &cached, "expand",
                             "2D/3D backing conversion");
                return {ExpandImage(image_info, cache_image_id), -1, -1};
            }
            return reject("different image type");
        }
        const bool cached_covers = cached.info.resources.Contains(image_info.resources);
        const bool request_covers = image_info.resources.Contains(cached.info.resources);
        if (!cached_covers && !request_covers) {
            // Taking a component-wise maximum here could invent guest memory beyond BOTH
            // allocations (e.g. more mips versus more layers). Preserve this legitimate alias.
            return reject("crossed mip/layer extents; neither image contains the other");
        }
        if (const auto depth = ResolveDepthOverlap(image_info, binding, cache_image_id, exact_fmt)) {
            TraceOverlap(image_info, binding, depth, &slot_images[depth],
                         depth == cache_image_id ? "view" : "expand", "depth/color binding");
            return {depth, -1, -1};
        }

        int mip{}, slice{};
        const auto reason = ReuseRejection(image_info, cached.info, binding, exact_fmt, mip, slice);
        if (reason.empty()) {
            TraceOverlap(image_info, binding, cache_image_id, &cached, "view", "covering backing");
            return {cache_image_id, mip, slice};
        }
        if (request_covers && image_info.IsCompatible(cached.info) &&
            cached.info.ViewRejection(image_info).empty() &&
            // Changing the number of layers relocates later guest mips. Only a one-mip
            // source, or an unchanged layer stride, can be preserved by CopyImage.
            (cached.info.resources.levels == 1 ||
             image_info.resources.layers == cached.info.resources.layers)) {
            TraceOverlap(image_info, binding, cache_image_id, &cached, "expand", reason);
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }
        return reject(reason);
    }

    if (!merged_image_id && image_info.guest_address > cached.info.guest_address) {
        int mip{}, slice{};
        const auto reason = ReuseRejection(image_info, cached.info, binding, exact_fmt, mip, slice);
        if (!reason.empty()) {
            return reject(reason);
        }
        TraceOverlap(image_info, binding, cache_image_id, &cached, "view",
                     "contained subresource", mip, slice);
        return {cache_image_id, mip, slice};
    }

    // Preserve a cached mip/array slice, including a currently bound render target, before
    // retiring it. Previously the target branch freed it before any larger image/copy existed.
    const int mip = cached.info.MipOf(image_info);
    const int slice = mip >= 0 ? cached.info.SliceOf(image_info, mip) : -1;
    if (mip < 0 || slice < 0 ||
        !cached.info.ViewRejection(image_info, {u32(mip), u32(slice)}).empty() ||
        !IsVulkanFormatCompatible(image_info.pixel_format, cached.info.pixel_format)) {
        return reject("cached image is not a covered child subresource");
    }
    if (!merged_image_id) {
        merged_image_id = slot_images.insert(instance, runtime, slot_image_views, image_info, &image_recycler);
        RegisterImage(merged_image_id);
        RefreshImage(slot_images[merged_image_id]);
    }
    // Inserting the parent may relocate SlotVector storage.
    auto& source = slot_images[cache_image_id];
    auto& merged = slot_images[merged_image_id];
    RefreshImage(source);
    runtime.CopyMip(&source, &merged, mip, slice);
    if (source.binding.is_bound || source.binding.is_target) {
        source.binding.needs_rebind = 1u;
    }
    merged.binding.is_target |= source.binding.is_target;
    TraceOverlap(image_info, binding, cache_image_id, &source, "merge",
                 "copy child before retiring backing", mip, slice);
    FreeImage(cache_image_id);
    TrackImage(merged_image_id);
    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    const auto new_info = info;
    const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, new_info, &image_recycler);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    RefreshImage(new_image);
    RefreshImage(src_image);
    runtime.CopyImage(&src_image, &new_image);
    new_image.binding.is_target = src_image.binding.is_target;

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);
    TrackImage(new_image_id);
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;
    ASSERT(info.guest_address != 0);

    std::scoped_lock lock{mutex};

    u64 key_hash = info.guest_address ^ info.guest_size << 17 ^ u64(info.pixel_format) << 40 ^
                   u64(info.size.width) << 24 ^ info.size.height ^ u64(desc.type) << 58 ^
                   u64(exact_fmt) << 63;
    key_hash = (key_hash ^ key_hash >> 33) * 0xFF51AFD7ED558CCDull;
    key_hash = (key_hash ^ key_hash >> 33) * 0xC4CEB9FE1A85EC53ull;
    key_hash ^= key_hash >> 33;
    auto& cached = find_image_cache[key_hash % find_image_cache.size()];
    // An exact match stays valid while that image is registered with the same description;
    // resolved overlaps (views into other images) only until any image registration changes.
    const auto still_exact = [&] {
        if (cached.view_mip >= 0 || cached.view_slice >= 0 || !cached.image_id ||
            !slot_images.is_allocated(cached.image_id)) {
            return false;
        }
        const Image& image = slot_images[cached.image_id];
        return True(image.flags & ImageFlagBits::Registered) &&
               image.info.guest_address == info.guest_address &&
               image.info.guest_size == info.guest_size && image.info.size == info.size &&
               image.info.pixel_format == info.pixel_format && image.info.type == info.type &&
               image.info.resources.Contains(info.resources) &&
               image.info.LayoutKey() == info.LayoutKey();
    };
    if ((cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()) &&
        cached.address == info.guest_address &&
        cached.size == info.guest_size && cached.extent == info.size &&
        cached.format == info.pixel_format && cached.type == info.type &&
        cached.exact_fmt == exact_fmt && cached.binding == desc.type &&
        cached.levels == info.resources.levels && cached.layers == info.resources.layers &&
        cached.layout_key == info.LayoutKey() &&
        !BbToggle::Disabled(BbToggle::FindImageCache)) {
        Image& image = slot_images[cached.image_id];
        image.NoteUse(scheduler.CurrentTick());
        TouchImage(image);
        if (cached.view_mip > 0) {
            desc.view_info.range.base.level += cached.view_mip;
        }
        if (cached.view_slice > 0) {
            desc.view_info.range.base.layer += cached.view_slice;
        }
        return cached.image_id;
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    int view_mip{-1};
    int view_slice{-1};
    // Preserve the exact-match fast path, but validate independent mip/layer coverage too.
    // Other covering parents go through resolution so modified child targets can be merged.
    for (const auto cache_id : image_ids) {
        const auto& candidate = slot_images[cache_id];
        if (candidate.info.pixel_format == vk::Format::eUndefined ||
            candidate.info.guest_address != info.guest_address ||
            candidate.info.guest_size != info.guest_size || candidate.info.size != info.size ||
            candidate.info.resources != info.resources) {
            continue;
        }
        int mip{}, slice{};
        const auto reason = ReuseRejection(info, candidate.info, desc.type, exact_fmt, mip, slice);
        if (reason.empty()) {
            image_id = cache_id;
            view_mip = mip ? mip : -1;
            view_slice = slice ? slice : -1;
            TraceOverlap(info, desc.type, cache_id, &candidate,
                         mip || slice ? "view" : "reuse", "covers requested resources", mip, slice);
            break;
        }
        TraceOverlap(info, desc.type, cache_id, &candidate, "reject reuse", reason, mip, slice);
    }

    if (!image_id) {
        for (const auto cache_id : image_ids) {
            const auto& candidate = slot_images[cache_id];
            if (cache_id == image_id || False(candidate.flags & ImageFlagBits::Registered)) {
                continue;
            }
            // Exact-format callers cannot end up with a differently formatted covering view.
            // A conversion/expansion may still create the requested format, so let it proceed.
            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            const auto [resolved, mip, slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id, exact_fmt);
            if (!resolved || resolved == image_id) {
                continue; // Keep the original subresource location on non-matches/child merges.
            }
            int original_mip{}, original_slice{};
            const auto reason = ReuseRejection(info, slot_images[resolved].info, desc.type,
                                               exact_fmt, original_mip, original_slice);
            if (!reason.empty()) {
                TraceOverlap(info, desc.type, resolved, &slot_images[resolved], "reject result", reason);
                continue;
            }
            image_id = resolved;
            view_mip = original_mip ? original_mip : -1;
            view_slice = original_slice ? original_slice : -1;
        }
    }
    // Create and register a new image
    if (!image_id) {
        view_mip = view_slice = -1;
        TraceOverlap(info, desc.type, {}, nullptr, "create", "no candidate covers requested view");
        image_id = slot_images.insert(instance, runtime, slot_image_views, info, &image_recycler);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.NoteUse(scheduler.CurrentTick());
    TouchImage(image);

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level += view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer += view_slice;
    }

    cached = FindImageCacheEntry{
        .address = info.guest_address,
        .size = info.guest_size,
        .extent = info.size,
        .format = info.pixel_format,
        .type = info.type,
        .exact_fmt = exact_fmt,
        .binding = desc.type,
        .levels = info.resources.levels,
        .layers = info.resources.layers,
        .layout_key = info.LayoutKey(),
        .generation = registry_generation.load(std::memory_order_relaxed),
        .image_id = image_id,
        .view_mip = view_mip,
        .view_slice = view_slice,
    };
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    if (!size) return {};
    ImageIds image_ids;
    // Every registered image is present in its first page's bucket. Only exact
    // base addresses qualify below, so later pages cannot add a candidate (#5240).
    ForEachImageInRegion(address, 1, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc, ViewMemo* memo,
                                     bool refresh) {
    Image& image = slot_images[image_id];
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8) &&
            image.info.guest_address != 0) {
            std::unique_lock lk{download_images_mutex};
            download_images.emplace(image_id);
            downloads_queued.store(true, std::memory_order_release);
        }
    }
    if (refresh) {
        UpdateImage(image_id);
    }
    if (memo && !BbToggle::Disabled(BbToggle::TextureViewMemo)) {
        if (memo->image_id == image_id && memo->backing == image.backing && memo->view_id) {
            return slot_image_views[memo->view_id];
        }
        ImageView& view = image.FindView(desc.view_info);
        const auto& ids = image.backing->image_view_ids;
        const u32 last = image.backing->last_view;
        memo->image_id = image_id;
        memo->backing = image.backing;
        memo->view_id = last < ids.size() && &slot_image_views[ids[last]] == &view ? ids[last]
                                                                                    : ids.back();
        return view;
    }
    return image.FindView(desc.view_info);
}

void TextureCache::RegisterMeta(VAddr address, MetaDataInfo info) {
    if (!address) return;
    const auto [it, inserted] = surface_metas.try_emplace(address, info);
    if (inserted) return;
    auto& current = it.value();
    if (current.type != info.type || current.owner_uid != info.owner_uid) {
        current = info;
    } else {
        current.owner_id = info.owner_id;
        current.num_samples = info.num_samples;
    }
}

void TextureCache::UnregisterMeta(VAddr address, u64 owner_uid) {
    const auto it = surface_metas.find(address);
    if (it != surface_metas.end() && it->second.owner_uid == owner_uid) surface_metas.erase(it);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && (!image.info.props.is_tiled || image.info.size.width <= 8)) {
        std::unique_lock lk{download_images_mutex};
        download_images.emplace(image_id);
        downloads_queued.store(true, std::memory_order_release);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    const auto cmask = desc.info.meta_info.cmask_addr;
    if (image.info.meta_info.cmask_addr != cmask) {
        UnregisterMeta(image.info.meta_info.cmask_addr, image.image_uid);
        image.info.meta_info.cmask_addr = cmask;
    }
    RegisterMeta(cmask, {.type = MetaType::CMask, .owner_id = image_id,
                         .owner_uid = image.image_uid, .num_samples = image.info.num_samples});

    const auto fmask = desc.info.meta_info.fmask_addr;
    if (image.info.meta_info.fmask_addr != fmask) {
        UnregisterMeta(image.info.meta_info.fmask_addr, image.image_uid);
        image.info.meta_info.fmask_addr = fmask;
    }
    RegisterMeta(fmask, {.type = MetaType::FMask, .owner_id = image_id,
                         .owner_uid = image.image_uid, .num_samples = image.info.num_samples});

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    const auto htile = desc.info.meta_info.htile_addr;
    if (image.info.meta_info.htile_addr != htile) {
        UnregisterMeta(image.info.meta_info.htile_addr, image.image_uid);
        image.info.meta_info.htile_addr = htile;
    }
    RegisterMeta(htile, {.type = MetaType::HTile, .owner_id = image_id,
                         .owner_uid = image.image_uid, .num_samples = image.info.num_samples,
                         .clear_mask = image.info.meta_info.htile_clear_mask});

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id = slot_images.insert(instance, runtime, slot_image_views, info, &image_recycler);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    return image.FindView(desc.view_info, false);
}

void TextureCache::RefreshImage(Image& image) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    BbStats::Timer timer{BbStats::t_refresh};
    const auto original_flags = image.flags;
    const bool new_backing = image.mip_hash_backing != image.backing ||
                             image.mip_hash_layout != image.info.LayoutKey();
    u64 diagnostic_uploaded = 0, diagnostic_unchanged = 0, diagnostic_hash_reused = 0;
    bool hash_checked = false; u32 hash_blockers = 0;
    const bool gpu_source = buffer_cache.IsRegionGpuModified(image.info.guest_address, image.info.guest_size);
    const char* outcome = "uploaded";
    if (BbStats::enabled) {
        ++image.refresh_count;
        image.refresh_source_bytes += image.info.guest_size;
    }
    SCOPE_EXIT {
        // Bounded per-process evidence. Powers of two show repeated objects
        // without turning every texture binding into a logging operation.
        static std::atomic<u32> events{0};
        const u64 count = image.refresh_count;
        const bool repeated = count >= 64 && (count & (count - 1)) == 0;
        if (BbStats::enabled && (repeated || diagnostic_uploaded >= (64ull << 20)) &&
            events.load(std::memory_order_relaxed) < 256 && events.fetch_add(1, std::memory_order_relaxed) < 256) {
            std::printf("Texture refresh: uid=%llu address=%#llx size=%llu count=%llu flags=%u "
                        "new_backing=%u source_total_mb=%.3f uploaded_mb=%.3f unchanged_mb=%.3f outcome=%s "
                        "gpu_source=%u gpu_writes=%llu write_address=%#llx write_size=%llu hash_reused_mb=%.3f "
                        "hash_check=%u hash_blockers=%#x hash_candidates=%llu hash_hits=%llu\n",
                        static_cast<unsigned long long>(image.image_uid),
                        static_cast<unsigned long long>(image.info.guest_address),
                        static_cast<unsigned long long>(image.info.guest_size),
                        static_cast<unsigned long long>(count), u32(original_flags), unsigned(new_backing),
                        image.refresh_source_bytes / 1e6, diagnostic_uploaded / 1e6,
                        diagnostic_unchanged / 1e6, outcome, unsigned(gpu_source),
                        static_cast<unsigned long long>(image.gpu_write_count),
                        static_cast<unsigned long long>(image.gpu_write_address),
                        static_cast<unsigned long long>(image.gpu_write_size), diagnostic_hash_reused / 1e6,
                        unsigned(hash_checked), hash_blockers,
                        static_cast<unsigned long long>(image.hash_reuse_candidates),
                        static_cast<unsigned long long>(image.hash_reuse_hits));
        }
    };


    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    auto* memory = Core::Memory::Instance();
    const auto mapping = memory->MappingGeneration();
    const auto backing_epoch = memory->BackingWriteGeneration();
    std::optional<u64> checked_whole_hash;
    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        const u64 hash = MaybeDirtyHash(image);
        if (image.hash == hash && False(image.flags & ImageFlagBits::GpuDirty)) {
            outcome = "whole_match";
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
        checked_whole_hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    static const bool keep_hashes = [] {
        const char* value = std::getenv("BB_KEEP_MIP_HASHES");
        return !value || value[0] != '0';
    }();
    static const bool parallel_hashes = [] {
        const char* value = std::getenv("BB_PARALLEL_IMAGE_HASH");
        return value && value[0] == '1';
    }();
    const auto layout = image.info.LayoutKey();
    const bool same_backing = image.mip_hash_backing == image.backing && image.mip_hash_layout == layout;
    static const bool reuse_gpu_hash = [] {
        const char* value = std::getenv("BB_REUSE_GPU_SOURCE_HASH");
        return !value || value[0] != '0';
    }();
    // Reuse only the CPU reference, NEVER the required GPU copy. Full guest
    // write protection, a clean CPU state, unchanged mappings/backing writes and
    // the exact uploaded backing/layout are all required. Unwatched modes hash.
    hash_blockers = BbHashReuse::Blockers({reuse_gpu_hash, WriteTracking(), is_gpu_dirty, gpu_source,
        same_backing, image.uploaded_mips == u16((1u << num_mips) - 1),
        False(image.flags & (ImageFlagBits::CpuDirty | ImageFlagBits::MaybeCpuDirty)),
        image.track_addr == image.info.guest_address && image.track_addr_end == image.info.guest_address + image.info.guest_size,
        image.cpu_hash_mapping != ~0ULL, !(mapping & 1), image.cpu_hash_mapping == mapping,
        backing_epoch != BbBackingWriteEpoch::Invalid, image.cpu_hash_backing_epoch != BbBackingWriteEpoch::Invalid,
        image.cpu_hash_backing_epoch == backing_epoch});
    hash_checked = true;
    const bool reuse_reference = hash_blockers == 0;
    if (BbStats::enabled) {
        image.hash_reuse_last_blockers = hash_blockers;
        if (is_gpu_dirty && gpu_source) {
            ++image.hash_reuse_candidates;
            image.hash_reuse_hits += unsigned(reuse_reference);
            BbHashReuse::Note(hash_blockers);
        }
    }
    std::array<u64, 16> hashes{};
    std::array<BbImageHash::Range, 16> ranges{};
    ASSERT(num_mips <= hashes.size());
    for (u32 m = 0; m < num_mips; ++m)
        ranges[m] = {image.info.mips_layout[m].offset, image.info.mips_layout[m].size};
    const auto* source = std::bit_cast<const u8*>(image.info.guest_address);
    const bool parallel = !reuse_reference && parallel_hashes && BbImageHash::SnapshotParallel(source, image.info.guest_size,
        std::span{ranges}.first(num_mips), std::span{hashes}.first(num_mips));

    u64 upload_bytes = 0;
    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // Protect GPU modified resources from accidental CPU reuploads.
        // bbport: every upload records the guest memory it saw, not only uploads of images
        // the GPU had already written. An image the GPU writes after an upload from the buffer
        // cache (GpuDirty: a storage image's first binding) or from a plain texture otherwise
        // had no reference, and the first CPU write anywhere in its page replaced the GPU's
        // contents with stale guest memory (a 1x1 exposure texture computed once: the
        // character creation preview went black after one frame).
        const bool reuse_whole = !parallel && checked_whole_hash && num_mips == 1 &&
                                 mip_offset == 0 && mip_size == image.info.guest_size;
        const u64 mip_hash = reuse_reference ? image.mip_hashes[m] : parallel ? hashes[m] : reuse_whole ? *checked_whole_hash :
            BbImageHash::Serial(source + mip_offset, mip_size);
        if (reuse_reference) {
            diagnostic_hash_reused += mip_size;
            BbStats::image_hash_reused_bytes.fetch_add(mip_size, std::memory_order_relaxed);
        }
        if (reuse_whole) BbStats::image_hash_reused_bytes.fetch_add(mip_size, std::memory_order_relaxed);
        hashes[m] = mip_hash;
        if ((keep_hashes || is_gpu_modified) && !is_gpu_dirty && same_backing &&
            (image.uploaded_mips & (1u << m)) && image.mip_hashes[m] == mip_hash) {
            diagnostic_unchanged += mip_size;
            BbStats::image_unchanged_bytes.fetch_add(mip_size, std::memory_order_relaxed);
            continue;
        }
        upload_bytes += mip_size;

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    diagnostic_uploaded = upload_bytes;
    if (image_copies.empty()) {
        outcome = "mips_match";
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    // Count source mips actually scheduled, excluding refresh attempts that only
    // clear dirty flags. Buffer bytes describe another stage of the same data.
    BbStats::image_upload_bytes.fetch_add(upload_bytes, std::memory_order_relaxed);

    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    const auto [buffer, offset] = tile_manager.DetileImage(in_buffer, in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    runtime.UploadImage(&image, buffer, image_copies);
    // Publish the reference only after all required uploads have been recorded.
    // A new backing/layout can never reuse an old backing's upload hashes.
    for (u32 m = 0; m < num_mips; ++m) image.mip_hashes[m] = hashes[m];
    image.uploaded_mips = u16((1u << num_mips) - 1);
    image.mip_hash_backing = image.backing;
    image.mip_hash_layout = layout;
    // A racing mapping/backing write invalidates the reference for the next
    // GPU-only refresh. CPU page faults remain serialized by the cache mutex.
    image.cpu_hash_mapping = WriteTracking() && mapping == memory->MappingGeneration() &&
        !(mapping & 1) && image.track_addr == image.info.guest_address &&
        image.track_addr_end == image.info.guest_address + image.info.guest_size ? mapping : ~0ULL;
    image.cpu_hash_backing_epoch = backing_epoch == memory->BackingWriteGeneration() ? backing_epoch : ~0ULL;
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth, float extra_lod_bias) {
    // Compare and plain uses of one S# need separate samplers; so do extra LOD biases.
    const auto key = MakeSamplerKey(instance, sampler, border_color_base, is_depth, extra_lod_bias);

    std::scoped_lock lock{samplers_mutex};
    const auto use = [&](Sampler& value) {
        sampler_lru_cache.Touch(value.lru_id, gc_tick);
        value.last_gpu_tick = std::max(value.last_gpu_tick, scheduler.CurrentTick());
        return value.Handle();
    };
    auto& memo = sampler_memo[(key.descriptor[0] ^ key.descriptor[2] ^ key.lod_bias ^ unsigned(key.compare)) % sampler_memo.size()];
    if (memo.sampler && memo.generation == sampler_generation && memo.key == key) {
        ++sampler_memo_hits;
        return use(*memo.sampler);
    }
    ++sampler_memo_misses;
    auto it = samplers.find(key);
    if (it == samplers.end()) {
        // Invalidate pointer memos before any operation that can relocate values.
        ++sampler_generation;
        it = samplers.try_emplace(key, instance, key).first;
        it.value().lru_id = sampler_lru_cache.Insert(key, gc_tick);
    }
    memo = {key, &it.value(), sampler_generation};
    return use(it.value());
}

void TextureCache::RegisterImage(ImageId image_id) {
    BbStats::images_registered.fetch_add(1, std::memory_order_relaxed);
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registry_generation;
    if (!instance.CanReportMemoryUsage()) {
        total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    }
    BbStats::live_image_bytes.fetch_add(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_add(1, std::memory_order_relaxed);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    image.lru_touched_tick = gc_tick;
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registry_generation;
    lru_cache.Free(image.lru_id);
    if (!instance.CanReportMemoryUsage()) {
        total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    }
    BbStats::live_image_bytes.fetch_sub(Common::AlignUp(image.info.guest_size, 1024),
                                        std::memory_order_relaxed);
    BbStats::live_images.fetch_sub(1, std::memory_order_relaxed);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.cpu_hash_mapping = ~0ULL; // Protection continuity was broken.
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.cpu_hash_mapping = ~0ULL; // Protection continuity was broken.
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    image.cpu_hash_mapping = ~0ULL; // Protection continuity was broken.
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::GarbageCollectImages() {
    const auto sample_now = std::chrono::steady_clock::now();
    // Submissions can arrive hundreds of times a second. Bound the driver queries and
    // eviction batches, allowing GPU-deferred destruction to retire between passes.
    if (memory_sample_time != std::chrono::steady_clock::time_point{} &&
        sample_now - memory_sample_time < std::chrono::milliseconds(250)) {
        return;
    }
    memory_sample_time = sample_now;
    SCOPE_EXIT { WriteMemorySample(); };
    if (instance.CanReportMemoryUsage()) {
        const auto snapshot = instance.GetDeviceMemorySnapshot();
        total_used_memory = snapshot.usage;
        driver_memory_budget = snapshot.budget;
        if (!snapshot.budget) {
            // Keep the last valid thresholds, but don't evict based on an unavailable query.
            buffer_cache.ReclaimMemory(memory_pressure, total_used_memory, false);
            return;
        }
        // Use the current budget on discrete GPUs too. The override is for reproducible
        // pressure tests; it changes cache policy, never the driver's allocation limit.
        static const u64 forced_budget = [] {
            const char* env = std::getenv("BB_GC_BUDGET_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : 0;
        }();
        gc_memory_budget = forced_budget ? forced_budget : snapshot.budget;
        memory_pressure.Update(total_used_memory, gc_memory_budget);
        if (gc_memory_budget) {
            trigger_gc_memory = memory_pressure.marks.trigger;
            pressure_gc_memory = memory_pressure.marks.pressure;
            critical_gc_memory = memory_pressure.marks.critical;
        }
    }
    BbStats::gc_used_bytes.store(total_used_memory, std::memory_order_relaxed);
    BbStats::gc_trigger_bytes.store(trigger_gc_memory, std::memory_order_relaxed);
    // bbport: gc_tick (one per guest submission, hundreds a second) at each of the last 64
    // seconds, for ages in seconds. The usage compared is all of our VRAM, not only images: on a
    // discrete GPU it stays over the trigger, where an age of 16 ticks (~3 frames) evicted every
    // texture briefly off screen and uploaded it again. Below the pressure mark an image goes
    // once unused for BB_GC_IDLE_SECONDS (20): the textures of an area left behind.
    const u64 second = u64(std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
    if (second != gc_second) {
        gc_second = second;
        BbStats::coarse_second.store(u32(second), std::memory_order_relaxed);
        gc_tick_at_second[second % gc_tick_at_second.size()] = gc_tick;
    }
    image_recycler.Budget(total_used_memory, driver_memory_budget,
                          instance.CanReportMemoryUsage() && memory_pressure.level == MemoryPressure::Level::Normal);
    // Both caches consume one process budget. Retire cold CPU-backed buffer copies
    // before evicting textures; only whole allocations actually freed count below.
    const u64 freed_buffers = buffer_cache.ReclaimMemory(
        memory_pressure, total_used_memory, instance.CanReportMemoryUsage() && driver_memory_budget != 0);
    if (BbToggle::Disabled(BbToggle::TextureCollector)) {
        return;
    }

    if (total_used_memory < trigger_gc_memory &&
        memory_pressure.level == MemoryPressure::Level::Normal) {
        return;
    }
    static const u64 idle_seconds = [] {
        const char* env = std::getenv("BB_GC_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 20, 1, 63);
    }();
    // The tick at the start of that second (an older one where no submission came then).
    const u64 idle_tick =
        gc_tick_at_second[(second - idle_seconds) % gc_tick_at_second.size()];
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 below_tick = 0;
    size_t num_deletions = 0;
    u32 visited = 0;
    // Batch-local estimate only: unregistering guest bytes is not a measurement of
    // freed VRAM. Host images may have different sizes, and destruction is deferred.
    u64 projected_usage = total_used_memory - std::min(total_used_memory, freed_buffers);
    const u64 pressure_target = instance.CanReportMemoryUsage()
                                   ? memory_pressure.marks.pressure_exit : pressure_gc_memory;
    const u64 critical_target = instance.CanReportMemoryUsage()
                                   ? memory_pressure.marks.critical_exit : critical_gc_memory;

    static const u64 pressure_idle_seconds = [] {
        const char* env = std::getenv("BB_GC_PRESSURE_IDLE_SECONDS");
        return std::clamp<u64>(env ? std::strtoull(env, nullptr, 10) : 5, 1, 63);
    }();
    const auto configure = [&](bool allow_aggressive, bool emergency = false) {
        pressured = instance.CanReportMemoryUsage()
                        ? memory_pressure.level != MemoryPressure::Level::Normal
                        : total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive &&
                    (instance.CanReportMemoryUsage()
                         ? memory_pressure.level == MemoryPressure::Level::Critical
                         : total_used_memory >= critical_gc_memory);
        const u64 ticks_to_destroy = std::min<u64>(aggresive ? 160 : pressured ? 80 : 16, gc_tick);
        below_tick = gc_tick - ticks_to_destroy;
        if (!pressured && !aggresive) {
            below_tick = std::min(below_tick, idle_tick);
        } else if (!emergency) {
            const u64 age = aggresive ? 1 : pressure_idle_seconds;
            below_tick = std::min(below_tick,
                gc_tick_at_second[(second - age) % gc_tick_at_second.size()]);
        }
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
        visited = 0;
    };

    static const bool tiled_writeback = [] {
        const char* env = std::getenv("BB_IMAGE_WRITEBACK_GC");
        const bool requested = env && std::strcmp(env, "1") == 0;
        if (requested) std::printf("Texture cache: experimental tiled GC %s (requires BB_WRITE_TRACKING=1)\n",
                                   WriteTracking() ? "enabled" : "disabled");
        return requested && WriteTracking();
    }();
    unsigned tiled_attempts = 0;
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0 || ++visited > 256) {
            return true;
        }
        auto& image = slot_images[image_id];
        const u64 now_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
            sample_now.time_since_epoch()).count();
        const u64 use=image.LastUse();
        if (!ImageLifetime::CanCollect(use,scheduler.CurrentTick(),
                !use || scheduler.IsFree(use),image.LastUseMs(),now_ms)) {
            lru_cache.Touch(image.lru_id,gc_tick);
            BbStats::gc_kept_images.fetch_add(1,std::memory_order_relaxed);
            return false;
        }
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        // Images whose GPU contents cannot be safely written back neither consume
        // the deletion quota nor stay first in line, so they cannot block later candidates.
        if (download && !pressured) {
            lru_cache.Touch(image.lru_id, gc_tick);
            BbStats::gc_kept_images.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        bool written_back = false;
        if (tiled && download) {
            if (!tiled_writeback || tiled_attempts || gc_tick < image.writeback_retry_tick) {
                ++gc_tiled_kept;
                lru_cache.Touch(image.lru_id, gc_tick);
                BbStats::gc_kept_images.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            ++tiled_attempts; // At most one attempt per 250 ms collector sample.
            if (!WriteBackTiledImage(image_id)) {
                image.writeback_retry_tick = gc_tick + 600;
                ++gc_tiled_kept;
                lru_cache.Touch(image.lru_id, gc_tick);
                return false;
            }
            written_back = true;
        }
        --num_deletions;
        if (download) {
            // bbport: synchronously, while the image still protects its pages. A deferred
            // write-back landed after FreeImage had unprotected them, over whatever the game
            // had meanwhile stored there (e.g. its heap after unloading an area).
            if (!written_back) DownloadImageMemory(image_id, true);
            ++gc_downloads;
            ++gc_total_downloads;
        }
        ++gc_evictions;
        ++gc_total_evictions;
        BbStats::gc_freed_images.fetch_add(1, std::memory_order_relaxed);
        u64 host_bytes = 0;
        for (const auto& backing : image.backing_images) host_bytes += backing.image.size_bytes;
        FreeImage(image_id);
        projected_usage = instance.CanReportMemoryUsage()
                              ? projected_usage - std::min(projected_usage, host_bytes)
                              : total_used_memory;

        if (projected_usage < critical_target) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && projected_usage < pressure_target) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    configure(false);
    lru_cache.ForEachItemBelow(below_tick, clean_up);

    if (projected_usage >= critical_target &&
        (instance.CanReportMemoryUsage()
             ? memory_pressure.level == MemoryPressure::Level::Critical
             : total_used_memory >= critical_gc_memory)) {
        configure(true);
        lru_cache.ForEachItemBelow(below_tick, clean_up);
        // If cold resources were insufficient, retain the existing bounded emergency pass.
        if (projected_usage >= critical_gc_memory) {
            configure(true, true);
            lru_cache.ForEachItemBelow(below_tick, clean_up);
        }
    }
    // bbport: evictions under memory pressure, at most every 5 s (BB_FRAME_STATS or not).
    if (pressured || gc_downloads != 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - gc_report_time >= std::chrono::seconds(5)) {
            std::printf("Texture cache: memory pressure, %llu of %llu MiB (critical %llu): "
                        "%llu images evicted, %llu written back since the last report\n",
                        (unsigned long long)(total_used_memory >> 20),
                        (unsigned long long)(pressure_gc_memory >> 20),
                        (unsigned long long)(critical_gc_memory >> 20),
                        (unsigned long long)gc_evictions, (unsigned long long)gc_downloads);
            gc_report_time = now;
            gc_evictions = gc_downloads = 0;
        }
    }
}

void TextureCache::WriteMemorySample() {
    // Optional low-frequency capture, independent of per-frame/GPU query sampling.
    if (!memory_log) {
        const char* path = std::getenv("BB_VRAM_LOG");
        if (!path || !*path) return;
        memory_log = std::fopen(path, "w");
        if (!memory_log) return;
        std::fprintf(memory_log, "t_s,driver_available,device_usage_bytes,device_budget_bytes,"
                                "gc_budget_bytes,tracked_image_bytes,guest_buffer_allocated_bytes,"
                                "guest_buffer_unused_bytes,pending_image_bytes,gc_evicted_images,"
                                "gc_written_back_images,gc_pressure,gc_critical,"
                                "pending_buffer_bytes,buffer_evicted_bytes,buffer_freed_bytes,"
                                "gc_tiled_attempts,gc_tiled_writebacks,gc_tiled_bytes,gc_tiled_kept,"
                                "gc_tiled_ms,gc_tiled_max_ms,image_recycle_bytes\n");
    }
    std::fprintf(memory_log, "%.3f,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.3f,%.3f,%llu\n",
                 std::chrono::duration<double>(memory_sample_time - memory_log_start).count(),
                 unsigned(instance.CanReportMemoryUsage()),
                 static_cast<unsigned long long>(total_used_memory),
                 static_cast<unsigned long long>(driver_memory_budget),
                 static_cast<unsigned long long>(gc_memory_budget),
                 static_cast<unsigned long long>(BbStats::vk_image_bytes.load()),
                 static_cast<unsigned long long>(BbStats::residency_alloc_bytes.load()),
                 static_cast<unsigned long long>(BbStats::residency_unused_bytes.load()),
                 static_cast<unsigned long long>(pending_image_bytes.load()),
                 static_cast<unsigned long long>(gc_total_evictions),
                 static_cast<unsigned long long>(gc_total_downloads),
                 unsigned(memory_pressure.level != MemoryPressure::Level::Normal),
                 unsigned(memory_pressure.level == MemoryPressure::Level::Critical),
                 static_cast<unsigned long long>(BbStats::pending_buffer_bytes.load()),
                 static_cast<unsigned long long>(BbStats::buffer_evicted_bytes.load()),
                 static_cast<unsigned long long>(BbStats::vram_chunks_freed_bytes.load()),
                 static_cast<unsigned long long>(gc_tiled_attempts),
                 static_cast<unsigned long long>(gc_tiled_writebacks),
                 static_cast<unsigned long long>(gc_tiled_bytes),
                 static_cast<unsigned long long>(gc_tiled_kept), gc_tiled_ms, gc_tiled_max_ms,
                 static_cast<unsigned long long>(image_recycler.Bytes()));
    std::fflush(memory_log);
}

void TextureCache::GarbageCollectSamplers() {
    std::scoped_lock lock{samplers_mutex};
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggresive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](const SamplerKey& key) {
        if (num_deletions == 0) {
            return true;
        }
        auto& sampler = samplers.at(key);
        if (sampler.last_gpu_tick >= scheduler.CurrentTick() ||
            !scheduler.IsFree(sampler.last_gpu_tick)) {
            sampler_lru_cache.Touch(sampler.lru_id, gc_tick);
            return false;
        }
        --num_deletions;
        const size_t lru_id = sampler.lru_id;
        ++sampler_generation; // Erase can also relocate other mapped values.
        samplers.erase(key);
        sampler_lru_cache.Free(lru_id);
        total_used_samplers = samplers.size();
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_samplers >= critical_gc_samplers) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };

    GarbageCollectImages();
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    if (image.lru_touched_tick == gc_tick) {
        return;
    }
    image.lru_touched_tick = gc_tick;
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        UnregisterMeta(meta_info.cmask_addr, image.image_uid);
    }
    if (meta_info.fmask_addr) {
        UnregisterMeta(meta_info.fmask_addr, image.image_uid);
    }
    if (meta_info.htile_addr) {
        UnregisterMeta(meta_info.htile_addr, image.image_uid);
    }

    {
        std::unique_lock lk{download_images_mutex};
        if (download_images.contains(image_id)) {
            download_images.erase(image_id);
        }
    }

    // Reclaim image and any image views it references.
    u64 allocated_bytes = 0;
    for (const auto& backing : image.backing_images) allocated_bytes += backing.image.size_bytes;
    pending_image_bytes.fetch_add(allocated_bytes, std::memory_order_relaxed);
    scheduler.DeferOperation([this, image_id, allocated_bytes] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
            if (False(image.flags & ImageFlagBits::GpuModified) && !image.scene_proxy &&
                !image.usage.vo_surface) image_recycler.Retire(std::move(backing.image));
        }
        slot_images.erase(image_id);
        pending_image_bytes.fetch_sub(allocated_bytes, std::memory_order_relaxed);
    });
}

} // namespace VideoCore
