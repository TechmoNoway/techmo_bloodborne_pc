// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    u32 mrt_mask;
    u32 motion_vectors = 0;
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
    };

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return stage_hashes == key.stage_hashes && vertex_buffer_formats == key.vertex_buffer_formats &&
            patch_control_points == key.patch_control_points && num_color_attachments == key.num_color_attachments &&
            color_buffers == key.color_buffers && blend_controls == key.blend_controls && write_masks == key.write_masks &&
            cb_shader_mask.raw == key.cb_shader_mask.raw && logic_op == key.logic_op && num_samples == key.num_samples &&
            depth_samples == key.depth_samples && color_samples == key.color_samples && mrt_mask == key.mrt_mask &&
            motion_vectors == key.motion_vectors && z_format == key.z_format && stencil_format == key.stencil_format &&
            depth_clamp_enable == key.depth_clamp_enable && prim_type == key.prim_type && polygon_mode == key.polygon_mode &&
            clip_space == key.clip_space && provoking_vtx_last == key.provoking_vtx_last && depth_clip_enable == key.depth_clip_enable;
    }

    auto CanonicalBytes() const noexcept {
        // Keep the v6 byte layout and the hashes of zero-padded old keys. C++
        // copies/lambda captures need not preserve unnamed bitfield padding.
        std::array<u8, sizeof(GraphicsPipelineKey)> bytes;
        std::memcpy(bytes.data(), this, bytes.size());
        const auto word = [&](size_t at, u32 value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); };
        static_assert(sizeof(Shader::PsColorBuffer) == 8 && offsetof(Shader::PsColorBuffer, swizzle) == 4);
        static_assert(sizeof(AmdGpu::BlendControl) == 4);
        for (size_t i = 0; i < color_buffers.size(); ++i) {
            const auto& c = color_buffers[i];
            word(offsetof(GraphicsPipelineKey, color_buffers) + i * sizeof(c),
                 u32(c.data_format) | u32(c.num_format) << 6 | u32(c.num_conversion) << 10 |
                 u32(c.export_format) << 13 | u32(c.blend_self_scale) << 17);
            const auto& b = blend_controls[i];
            word(offsetof(GraphicsPipelineKey, blend_controls) + i * sizeof(b),
                 u32(b.color_src_factor) | u32(b.color_func) << 5 | u32(b.color_dst_factor) << 8 |
                 u32(b.alpha_src_factor) << 16 | u32(b.alpha_func) << 21 | u32(b.alpha_dst_factor) << 24 |
                 u32(b.separate_alpha_blend) << 29 | u32(b.enable) << 30 | u32(b.disable_rop3) << 31);
        }
        constexpr auto samples_end = offsetof(GraphicsPipelineKey, color_samples) + sizeof(color_samples);
        std::fill(bytes.begin() + samples_end, bytes.begin() + offsetof(GraphicsPipelineKey, mrt_mask), u8{});
        constexpr auto tail = offsetof(GraphicsPipelineKey, motion_vectors) + sizeof(motion_vectors);
        static_assert(tail + 8 <= sizeof(GraphicsPipelineKey));
        word(tail, u32(z_format) | u32(stencil_format) << 2 | u32(depth_clamp_enable) << 3);
        word(tail + 4, u32(prim_type) | u32(polygon_mode) << 5 | u32(clip_space) << 7 |
                      u32(provoking_vtx_last) << 8 | u32(depth_clip_enable) << 9);
        std::fill(bytes.begin() + tail + 8, bytes.end(), u8{});
        return bytes;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar, bool legacy);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading, bool speculative = false, NativeSchedule schedule = {});
    ~GraphicsPipeline();

    // The background constructor reads owned snapshots; publishing a completed
    // pipeline binds it to the foreground's verified, persistent program Infos.
    void BindRuntimeStages(std::span<const Shader::Info*, MaxShaderStages> infos) {
        std::ranges::copy(infos, stages.begin());
    }

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    /// Gets the attributes and bindings for vertex inputs.
    /// bbport: `sharps`, when given, are the V#s of the fetch shader's attributes as the GPU
    /// command thread read them (in the draw packet): the V# tables they come from may have been
    /// rewritten by a later constant engine dump by the time the recording thread gets here.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1, std::span<const AmdGpu::Buffer> sharps = {}) const;

private:
    void BuildDescSetLayout(bool preloading, bool speculative);

private:
    GraphicsPipelineKey key;
    vk::UniqueDescriptorSetLayout motion_address_layout;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        const auto bytes = key.CanonicalBytes();
        return XXH3_64bits(bytes.data(), bytes.size());
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
