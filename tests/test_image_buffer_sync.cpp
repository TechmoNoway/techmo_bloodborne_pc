// SPDX-License-Identifier: GPL-2.0-or-later
// Real GPU producer -> VRAM buffer -> image consumer, without a game or window.
// Checks the production transfers/barriers, not the unavailable Windows in-place model.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "common/slot_vector.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

int main() {
    using namespace VideoCore;
    Vulkan::Instance instance{-1, false};
    Vulkan::Scheduler scheduler{instance, false};
    Vulkan::Runtime runtime{instance, scheduler};
    Common::SlotVector<ImageView> views;
    Common::SlotVector<Image> images;
    ImageInfo info{};
    info.type = AmdGpu::ImageType::Color2D;
    info.pixel_format = vk::Format::eR8G8B8A8Unorm;
    info.size = {8, 4, 1};
    info.pitch = 8;
    info.num_bits = 32;
    info.resources = {1, 1};
    const auto producer_id = images.insert(instance, runtime, views, info);
    const auto consumer_id = images.insert(instance, runtime, views, info);
    auto& producer = images[producer_id];
    auto& consumer = images[consumer_id];
    constexpr u32 Offset = 128, PixelBytes = 8 * 4 * 4, BufferBytes = 512;
    Buffer vram{instance, 0, BufferBytes, MemoryType::DeviceLocal, "Image sync VRAM"};
    Buffer readback{instance, 0, BufferBytes, MemoryType::HostCached, "Image sync readback"};
    Buffer consumed{instance, 0, PixelBytes, MemoryType::HostCached, "Image sync consumer"};
    const vk::BufferImageCopy via_vram{
        .bufferOffset = Offset, .bufferRowLength = 8, .bufferImageHeight = 4,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {8, 4, 1},
    };
    auto packed = via_vram;
    packed.bufferOffset = 0;
    const vk::BufferCopy copy{0, 0, BufferBytes};
    for (u32 rgba : {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu}) {
        runtime.FillBuffer(&vram, 0, BufferBytes, 0xa5a5a5a5u);
        vk::ClearValue clear{};
        clear.color.float32 = std::array{float(rgba & 255) / 255,
            float((rgba >> 8) & 255) / 255, float((rgba >> 16) & 255) / 255, 1.f};
        runtime.ClearImage(&producer, {}, clear);
        runtime.DownloadImage(&producer, &vram, std::span{&via_vram, 1});
        // A GPU consumer reads the freshly written buffer before any host wait/readback.
        runtime.UploadImage(&consumer, &vram, std::span{&via_vram, 1});
        runtime.CopyBuffer(&vram, &readback, std::span{&copy, 1});
        runtime.DownloadImage(&consumer, &consumed, std::span{&packed, 1});
        scheduler.Finish();
        readback.Invalidate(0, BufferBytes);
        consumed.Invalidate(0, PixelBytes);
        for (u32 at = 0; at < BufferBytes; at += 4) {
            u32 actual;
            std::memcpy(&actual, readback.mapped_data.data() + at, sizeof(actual));
            assert(actual == (at >= Offset && at < Offset + PixelBytes ? rgba : 0xa5a5a5a5u));
        }
        for (u32 at = 0; at < PixelBytes; at += 4) {
            u32 actual;
            std::memcpy(&actual, consumed.mapped_data.data() + at, sizeof(actual));
            assert(actual == rgba);
        }
        std::printf("PASS: GPU image -> VRAM +%u -> GPU image, frame color %08x\n", Offset, rgba);
    }
    puts("PASS: image/buffer command order, repeated submissions and untouched buffer guards");
}
