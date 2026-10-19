// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "video_core/amdgpu/regs_texture.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace VideoCore {

struct SamplerKey {
    std::array<u32, 4> descriptor{};
    std::array<u32, 4> border{};
    u32 lod_bias{};
    bool compare{};
    bool operator==(const SamplerKey&) const = default;
};
struct SamplerKeyHash {
    size_t operator()(const SamplerKey& key) const noexcept;
};
SamplerKey MakeSamplerKey(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                         AmdGpu::BorderColorBuffer border, bool is_depth, float extra_lod_bias);

class Sampler {
public:
    explicit Sampler(const Vulkan::Instance& instance, const SamplerKey& key);
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    Sampler(Sampler&&) = default;
    Sampler& operator=(Sampler&&) = default;

    vk::Sampler Handle() const noexcept {
        return *handle;
    }

    size_t lru_id{};
    u64 last_gpu_tick{};

private:
    vk::UniqueSampler handle;
};

} // namespace VideoCore
