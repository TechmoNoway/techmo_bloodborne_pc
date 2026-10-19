// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <xxhash.h>
#include "common/hash.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

size_t SamplerKeyHash::operator()(const SamplerKey& key) const noexcept {
    auto hash = HashCombine(XXH3_64bits(key.descriptor.data(), sizeof(key.descriptor)),
                            XXH3_64bits(key.border.data(), sizeof(key.border)));
    return HashCombine(hash, (u64{key.lod_bias} << 1) | unsigned(key.compare));
}

SamplerKey MakeSamplerKey(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                         AmdGpu::BorderColorBuffer border, bool is_depth, float extra_lod_bias) {
    SamplerKey key{};
    static_assert(sizeof(sampler) == sizeof(key.descriptor));
    std::memcpy(key.descriptor.data(), &sampler, sizeof(sampler));
    key.lod_bias = std::bit_cast<u32>(extra_lod_bias);
    key.compare = is_depth;
    if (sampler.border_color_type == AmdGpu::BorderColor::Custom &&
        instance.IsCustomBorderColorSupported()) {
        const auto* colors = border.Address<std::array<u32, 4>*>();
        // Same guest read protocol as sampler creation, now captured once so
        // cache equality and Vulkan creation consume the identical color.
        std::memcpy(key.border.data(), &colors[sampler.border_color_ptr.Value()], sizeof(key.border));
    }
    return key;
}

Sampler::Sampler(const Vulkan::Instance& instance, const SamplerKey& key) {
    const auto sampler = std::bit_cast<AmdGpu::Sampler>(key.descriptor);
    const bool is_depth = key.compare;
    const float extra_lod_bias = std::bit_cast<float>(key.lod_bias);
    using namespace Vulkan;
    const bool anisotropy_enable = instance.IsAnisotropicFilteringSupported() &&
                                   (AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                    AmdGpu::IsAnisoFilter(sampler.xy_min_filter));
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(sampler.MaxAniso(), 1.0f, instance.MaxSamplerAnisotropy())
                          : 1.0f;
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto custom_border_color_array = std::bit_cast<std::array<float, 4>>(key.border);

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = std::clamp(sampler.LodBias() + extra_lod_bias, -instance.MaxSamplerLodBias(),
                                 instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = sampler.MinLod(),
        .maxLod = sampler.MaxLod(),
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore
