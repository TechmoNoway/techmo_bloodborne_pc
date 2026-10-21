// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_set>
#include <ranges>
#include "common/serdes.h"
#include "common/hash.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/compile_inputs.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/pipeline_replay_metrics.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 13u; // bbport: guest early depth and indirect ID semantics
static constexpr u32 MotionShaderBinaryVersion = 14u; // same fixes; runtime address descriptor
static constexpr u32 ShaderMetaVersion = 8u; // new fragment/vertex specialization flags
static constexpr u32 PipelineKeyVersion = 6u; // pointer-free pipeline state + generated fragment
} // namespace Serialization

namespace Shader::Cache {
std::vector<u8> Metadata(const Info& info, const StageSpecialization& spec, size_t permutation) {
    Serialization::Archive ar;
    Serialization::Writer w{ar};
    w.Write(Serialization::ShaderMetaVersion);
    w.Write(info.hw_stage == HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors
        ? Serialization::MotionShaderBinaryVersion : Serialization::ShaderBinaryVersion);
    w.Write(HashCombine(info.pgm_hash, permutation)); w.Write(permutation);
    spec.Serialize(ar); info.Serialize(ar);
    return ar.TakeOff();
}
}

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    const bool vertex_motion = info.hw_stage == Shader::HwStage::Vertex &&
                               spec.runtime_info.hw.vs.motion_vectors;
    meta.Write(vertex_motion ? Serialization::MotionShaderBinaryVersion
                             : Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx, u64 expected_hash) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion &&
        binary_version != Serialization::MotionShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    if (!spec.Deserialize(ar) || !info.Deserialize(ar)) {
        return false;
    }
    if (perm_idx >= 4096 || perm_hash_ar != expected_hash ||
        HashCombine(info.pgm_hash, perm_idx) != expected_hash) {
        return false;
    }

    // Both binary classes require the current translator. Motion binaries also load
    // the current addresses from a renderer-owned descriptor instead of embedding them.
    const bool vertex_motion = info.hw_stage == Shader::HwStage::Vertex &&
                               spec.runtime_info.hw.vs.motion_vectors;
    if (binary_version != (vertex_motion ? Serialization::MotionShaderBinaryVersion
                                        : Serialization::ShaderBinaryVersion)) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    ReplayMetrics::Scope decode{ReplayMetrics::Phase::RecordDecode};
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);
    decode.Stop();

    std::vector<u8> meta_blob;
    {
        ReplayMetrics::Scope read{ReplayMetrics::Phase::MetadataRead};
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", compute_key.value), meta_blob);
    }
    if (meta_blob.empty()) {
        last_cache_rejection = "missing_metadata";
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0, compute_key.value)) return false;
    if (sel.infos[0]->hw_stage != Shader::HwStage::Compute) {
        last_cache_rejection = "stage_mismatch"; return false;
    }

    if (compute_pipelines.contains(compute_key)) {
        return true;
    }
    auto pipeline =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);
    compute_pipelines.emplace(compute_key, std::move(pipeline));

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer data{ar};
    data.Write(vertex_attributes);
    data.Write(vertex_bindings);
    data.Write(divisors);
    // No pNext, pSampleMask, or Vulkan handles belong in a persistent record.
    ASSERT(!multisampling.pNext && !multisampling.pSampleMask);
    data.Write(multisampling.rasterizationSamples);
    data.Write(multisampling.sampleShadingEnable);
    data.Write(multisampling.minSampleShading);
    data.Write(multisampling.alphaToCoverageEnable);
    data.Write(multisampling.alphaToOneEnable);
    data.Write(tcs);
    data.Write(tes);
    data.Write(fragment);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar, bool legacy) {
    Serialization::Reader data{ar};
    if (legacy) {
        // Read old v5 records without ever using persisted pointers. Current writers use v6.
        data.Read(&vertex_attributes, sizeof(vertex_attributes));
        data.Read(&vertex_bindings, sizeof(vertex_bindings));
        data.Read(&divisors, sizeof(divisors));
        if (vertex_attributes.size() > vertex_attributes.capacity() ||
            vertex_bindings.size() > vertex_bindings.capacity() ||
            divisors.size() > divisors.capacity()) {
            throw std::runtime_error("Invalid cached vertex input count");
        }
        data.Read(multisampling);
        if (multisampling.pNext || multisampling.pSampleMask) {
            return false;
        }
    } else {
        data.Read(vertex_attributes);
        data.Read(vertex_bindings);
        data.Read(divisors);
        data.Read(multisampling.rasterizationSamples);
        data.Read(multisampling.sampleShadingEnable);
        data.Read(multisampling.minSampleShading);
        data.Read(multisampling.alphaToCoverageEnable);
        data.Read(multisampling.alphaToOneEnable);
    }
    data.Read(tcs);
    data.Read(tes);
    if (!legacy) {
        data.Read(fragment);
    }
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar, bool legacy) {
    ReplayMetrics::Scope decode{ReplayMetrics::Phase::RecordDecode};
    sel.graphics_key.Deserialize(ar);
    if (sel.graphics_key.motion_vectors &&
        !instance.GetPhysicalDevice().getFeatures().vertexPipelineStoresAndAtomics) {
        last_cache_rejection = "unsupported_motion_feature";
        return false;
    }

    GraphicsPipeline::SerializationSupport sdata{};
    if (!sdata.Deserialize(ar, legacy)) {
        return false;
    }
    decode.Stop();
    // v5 did not save the generated clip-discard fragment shader. Only this ambiguous
    // legacy variant needs rebuilding; ordinary v5 pipelines remain usable.
    if (legacy && profile.needs_clip_distance_emulation &&
        !sel.graphics_key.stage_hashes[u32(Shader::SwStage::Fragment)]) {
        last_cache_rejection = "legacy_auxiliary_missing";
        return false;
    }

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        {
            ReplayMetrics::Scope read{ReplayMetrics::Phase::MetadataRead};
            Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                               fmt::format("{:#018x}", hash), meta_blob);
        }
        if (meta_blob.empty()) {
            last_cache_rejection = "missing_metadata";
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx, hash)) return false;
        if (u32(sel.infos[stage_idx]->sw_stage) != stage_idx) {
            last_cache_rejection = "stage_mismatch"; return false;
        }
    }

    if (graphics_pipelines.contains(sel.graphics_key)) {
        return true;
    }
    auto pipeline = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);
    graphics_pipelines.emplace(sel.graphics_key, std::move(pipeline));

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage, u64 expected_hash) {
    auto& database = Storage::DataBase::Instance();
    const auto name = fmt::format("{:#018x}", expected_hash);
    std::vector<u8> stamp;
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        ReplayMetrics::Scope read{ReplayMetrics::Phase::MetadataRead};
        database.Load(Storage::BlobType::ShaderCompiler, name, stamp);
        if (!stamp.empty() && std::string(stamp.begin(), stamp.end()) != Shader::Cache::CompilerKey()) {
            last_cache_rejection = "compiler_mismatch"; return false;
        }
        if (stamp.empty() && require_compiler_stamp) {
            last_cache_rejection = "missing_compiler_stamp"; return false;
        }
    }
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    std::optional<Shader::Gcn::FetchShaderData> fetch;
    size_t perm_idx{};
    {
        ReplayMetrics::Scope decode{ReplayMetrics::Phase::MetadataDecode};
        if (!LoadShaderMeta(ar, program->info, fetch, spec, perm_idx, expected_hash)) {
            last_cache_rejection = "incompatible_metadata"; return false;
        }
    }

    std::vector<u32> spv;
    {
        ReplayMetrics::Scope read{ReplayMetrics::Phase::SpirvRead};
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
            fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx), spv);
    }
    ReplayMetrics::Scope validate{ReplayMetrics::Phase::SpirvValidate};
    if (spv.size() < 5 || spv[0] != 0x07230203) {
        last_cache_rejection = "invalid_spirv";
        return false;
    }
    // Reject truncated instructions before passing cached SPIR-V to the driver.
    for (size_t word = 5; word < spv.size();) {
        const u32 count = spv[word] >> 16;
        if (!count || count > spv.size() - word) {
            last_cache_rejection = "invalid_spirv";
            return false;
        }
        word += count;
    }
    validate.Stop();

    auto it = program_cache.find(program->info.pgm_hash);
    Program* target = it == program_cache.end() ? program.get() : it->second.get();
    vk::ShaderModule module;
    bool created_module = false;
    if (perm_idx < target->modules.size() && target->modules[perm_idx].module) {
        // The persisted index is part of the pipeline key. Equal specializations at other
        // indices are not conflicts: equality deliberately ignores some unused bindings.
        if (!(target->modules[perm_idx].spec == spec)) {
            last_cache_rejection = "specialization_conflict";
            return false;
        }
        module = target->modules[perm_idx].module;
    } else {
        ReplayMetrics::Scope create{ReplayMetrics::Phase::ShaderModule};
        const auto [result, compiled] = instance.GetDevice().createShaderModule(
            {.codeSize = spv.size() * sizeof(u32), .pCode = spv.data()});
        if (result != vk::Result::eSuccess) {
            throw Serialization::CorruptData{"cached SPIR-V rejected by the driver"};
        }
        module = compiled;
        created_module = true;
    }
    // Rebind before moving: a temporary Program may have supplied this permutation's Info.
    // Never retain a pointer to that temporary in a cached StageSpecialization.
    spec.info = &target->info;
    sel.runtime_infos[stage] = spec.runtime_info;
    if (fetch) {
        sel.fetch_shader = std::move(fetch);
    }
    target->InsertPermut(module, std::move(spec), perm_idx);
    if (!target->info_template) {
        target->info_template = std::make_unique<Shader::Info>(target->info);
    }
    sel.infos[stage] = &target->info;
    sel.modules[stage] = module;
    if (it == program_cache.end()) {
        const auto hash = program->info.pgm_hash;
        program_cache.emplace(hash, std::move(program));
    }
    if (created_module) warmup_modules.push_back({target->info.pgm_hash, perm_idx, module});
    if (stamp.empty() && !EmulatorSettings.IsPipelineCacheArchived()) {
        const auto key = Shader::Cache::CompilerKey();
        database.Save(Storage::BlobType::ShaderCompiler, name, std::vector<u8>{key.begin(), key.end()});
    }
    return true;
}

void PipelineCache::RollbackWarmupModules() {
    for (const auto& entry : warmup_modules | std::views::reverse) {
        const auto found = program_cache.find(entry.hash);
        if (found == program_cache.end()) continue;
        auto& modules = found->second->modules;
        if (entry.permutation < modules.size() && modules[entry.permutation].module == entry.module) {
            instance.GetDevice().destroyShaderModule(entry.module);
            // A surviving higher permutation keeps this slot in the vector.
            // Invalidate its specialization as well as its handle; persistent
            // pipeline keys require the remaining indices to stay unchanged.
            modules[entry.permutation] = {};
            while (!modules.empty() && !modules.back().module) modules.pop_back();
            if (modules.empty()) program_cache.erase(found);
        }
    }
    warmup_modules.clear();
}

void PipelineCache::WarmUp() {
    warmup_evidence.Reset("disabled");
    {
        std::scoped_lock lock{progress_mutex};
        precache_progress = {};
    }
    const auto complete = [] {
        std::scoped_lock lock{progress_mutex};
        precache_progress.rejected = precache_progress.total - precache_progress.loaded;
        precache_progress.complete = true;
    };
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        complete();
        return;
    }

    Storage::DataBase::Instance().Open();
    warmup_evidence.Reset("unavailable");
    if (!Storage::DataBase::Instance().IsOpened()) {
        complete();
        return;
    }
    auto& database = Storage::DataBase::Instance();
    const auto names = database.ListBlobs(Storage::BlobType::PipelineKey);
    {
        std::scoped_lock lock{progress_mutex};
        precache_progress.total = static_cast<u32>(names.size());
    }

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        warmup_evidence.Reset("profile_missing");
        // Without a profile the remaining records have no proven shader-feature compatibility.
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        complete();
        return;
    }
    const bool legacy_profile = !profile.native_constants &&
        profile_data.size() == offsetof(Shader::Profile, native_constants);
    if (profile_data.size() != sizeof(Shader::Profile) && !legacy_profile) {
        LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                    profile_data.size(), sizeof(Shader::Profile));
    }
    Shader::Profile cached_profile{};
    if (profile_data.size() == sizeof(Shader::Profile) || legacy_profile) {
        std::memcpy(&cached_profile, profile_data.data(), profile_data.size());
    }
    if ((!legacy_profile && profile_data.size() != sizeof(Shader::Profile)) || cached_profile != profile) {
        warmup_evidence.Reset("profile_mismatch");
        // bbport: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        complete();
        return;
    }

    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        std::vector<u8> stamp;
        if (!legacy_profile && !database.IsReadOnly()) Shader::Cache::RebuildBeforeWarmUp(database.Path());
        database.Load(Storage::BlobType::ShaderCompiler, "translator", stamp);
        require_compiler_stamp = !stamp.empty();
    }
    const auto* regs = sel.regs;
    warmup_evidence.Reset("ready");
    u32 damaged = 0;
    std::map<std::string_view, u32> rejections;
    // Sequential by design: selection, programs, SRT walker allocation and archive reads are
    // shared. A worker pool needs isolated reconstruction state before it can be safe.
    for (const auto& name : names) {
        bool loaded = false;
        last_cache_rejection = "invalid_record";
        warmup_modules.clear();
        sel = {.regs = regs}; // Clear partial state after rejection, including runtime/fetch.
        try {
            std::vector<u8> data;
            database.Load(Storage::BlobType::PipelineKey, name, data);
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader reader{ar};
            u32 version{}, is_compute{};
            reader.Read(version);
            reader.Read(is_compute);
            if ((version == 5 || version == Serialization::PipelineKeyVersion) && is_compute <= 1) {
                loaded = is_compute ? LoadComputePipeline(ar)
                                    : LoadGraphicsPipeline(ar, version == 5);
                if (loaded) {
                    const auto actual = is_compute ? fmt::format("c_{:#018x}", compute_key.value)
                        : fmt::format("g_{:#018x}", std::hash<GraphicsPipelineKey>{}(sel.graphics_key));
                    warmup_evidence.Record(actual, true, "none");
                }
            } else {
                last_cache_rejection = "record_version_or_type";
            }
        } catch (const std::exception& error) {
            last_cache_rejection = "record_exception";
            ++damaged;
            LOG_WARNING(Render, "Skipping pipeline {}: {}", name, error.what());
        }
        warmup_evidence.Record(name, loaded, last_cache_rejection);
        if (!loaded) {
            ++rejections[last_cache_rejection];
            RollbackWarmupModules();
        }
        std::scoped_lock lock{progress_mutex};
        loaded ? ++precache_progress.loaded : ++precache_progress.rejected;
    }
    warmup_modules.clear();
    sel = {.regs = regs};
    if (damaged) {
        LOG_WARNING(Render, "Ignored {} damaged pipeline records; compatible entries retained", damaged);
    }
    const auto status = GetPrecacheProgress();
    LOG_INFO(Render, "Preloaded {} of {} pipelines ({} rejected)", status.loaded, status.total,
             status.rejected);
    for (const auto& [reason, count] : rejections)
        std::printf("Pipeline warmup rejected: %.*s %u\n", int(reason.size()), reason.data(), count);
    database.FinishPreload();
    if (legacy_profile) {
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        database.Save(Storage::BlobType::ShaderProfile, "profile", std::move(profile_data));
        std::puts("Pipeline cache profile: migrated legacy standard profile; existing records retained");
    }
    complete();
}

void PipelineCache::Sync() {
    if (offline_replay) return;
    for (const auto& [_, pipeline] : graphics_pipelines) if (pipeline) pipeline->CompleteNative();
    for (const auto& [_, pipeline] : compute_pipelines) if (pipeline) pipeline->CompleteNative();
    if (precompiler) {
        precompiler->Stop();
        std::printf("Pipeline preparation: %zu queued, %llu adopted, %.2f ms foreground waits, %zu retained\n",
            precompiler->Queued(), static_cast<unsigned long long>(prepared_pipeline_hits),
            prepared_pipeline_wait_ns / 1e6, precompiler->Outstanding());
        std::printf("Pipeline required submissions: gfx %zu\n", precompiler->RequiredQueued());
    }
    if (compute_precompiler) {
        compute_precompiler->Stop();
        std::printf("Compute pipeline preparation: %zu queued, %llu adopted, %.2f ms foreground waits, %zu retained\n",
            compute_precompiler->Queued(), static_cast<unsigned long long>(prepared_compute_hits),
            prepared_compute_wait_ns / 1e6, compute_precompiler->Outstanding());
        std::printf("Pipeline required submissions: compute %zu\n", compute_precompiler->RequiredQueued());
    }
    std::scoped_lock lock{native_cache_mutex};
    try {
        if (preparation_cache) {
            const auto source = *preparation_cache;
            const auto status = instance.GetDevice().mergePipelineCaches(*pipeline_cache, source);
            if (status != vk::Result::eSuccess)
                LOG_WARNING(Render, "Cannot merge prepared driver cache: {}", vk::to_string(status));
        }
        if (compute_preparation_cache) {
            const auto source = *compute_preparation_cache;
            const auto status = instance.GetDevice().mergePipelineCaches(*pipeline_cache, source);
            if (status != vk::Result::eSuccess)
                LOG_WARNING(Render_Vulkan, "Cannot merge prepared compute cache: {}", vk::to_string(status));
        }
        SaveNativeCache();
        Storage::DataBase::Instance().Close();
    } catch (const std::exception& error) {
        LOG_WARNING(Render, "Pipeline cache shutdown save failed: {}", error.what());
    }
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};
    // The read-only diagnostics database has no guest process. Retaining host
    // walkers there would also exhaust the fixed JIT arena across replay rounds.
    const bool restore_host_code = !Storage::DataBase::Instance().IsReadOnly();

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        // bbport: the size is checked before the code is registered (it becomes executable):
        // a cut-short file must not have bytes past its end run as the walker.
        const auto code = ar.CurrPtr();
        ar.Advance(walker_func_size);
        walker_func = restore_host_code ? RegisterWalkerCode(code, walker_func_size) : nullptr;
    }
    if (!restore_host_code) {
        walker_func = nullptr;
        walker_func_size = 0;
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    auto persistent_runtime = runtime_info;
    if (persistent_runtime.hw_stage == HwStage::Geometry) persistent_runtime.hw.gs.vs_copy = {};
    spec.Write(persistent_runtime);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);
    if (runtime_info.hw_stage == HwStage::Geometry) runtime_info.hw.gs.vs_copy = {};

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        if (fetch_data_size != sizeof(Gcn::FetchShaderData)) {
            return false;
        }
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
