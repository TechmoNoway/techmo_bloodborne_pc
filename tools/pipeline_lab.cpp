// SPDX-License-Identifier: GPL-2.0-or-later
// Headless, read-only reconstruction through the production cache loader.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <crtdbg.h>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <windows.h>
#include "common/serdes.h"
#include "common/hash.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/compile_inputs.h"
#include "shader_recompiler/constant_buffer.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/pipeline_replay_metrics.h"

namespace {
std::string Quote(std::string_view s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32) out += fmt::format("\\u{:04x}", c);
        else out += c;
    }
    return out + '"';
}
std::string Hex(std::span<const u8> data) {
    std::string out;
    out.reserve(data.size() * 2);
    for (auto byte : data) out += fmt::format("{:02x}", byte);
    return out;
}
void Line(std::ofstream& out, const std::string& line) {
    out << line << '\n';
    out.flush(); // Preserve the last attempted key if the driver/process aborts.
    if (!out) throw std::runtime_error("Cannot write replay report");
}
}

namespace Vulkan {
struct PipelineReplay {
    struct Variant {
        std::unique_ptr<Shader::Info> info;
        Shader::StageSpecialization spec;
        std::string binary;
    };
    struct VariantAudit {
        std::unordered_set<u64> seen;
        std::unordered_map<u64, std::vector<Variant>> programs;
        void Record(const PipelineCache& cache, std::ofstream& out) {
            for (const auto& [program_hash, program] : cache.program_cache) {
                for (size_t i = 0; i < program->modules.size(); ++i) {
                    const auto& module = program->modules[i];
                    const auto stage_hash = HashCombine(program_hash, i);
                    if (!module.module || !seen.insert(stage_hash).second) continue;
                    Variant owned{std::make_unique<Shader::Info>(program->info), module.spec,
                                  fmt::format("{:#018x}_{}.spv", program_hash, i)};
                    owned.spec.info = owned.info.get();
                    std::string equal, compared;
                    auto& prior = programs[program_hash];
                    // Equality is a lookup compatibility relation and can be directional.
                    // Report only mutual matches; this still does not prove merge safety.
                    for (size_t n = 0; n < std::min(prior.size(), size_t{256}); ++n) {
                        if (!compared.empty()) compared += ',';
                        compared += Quote(prior[n].binary);
                        if (owned.spec == prior[n].spec && prior[n].spec == owned.spec) {
                            if (!equal.empty()) equal += ',';
                            equal += Quote(prior[n].binary);
                        }
                    }
                    // Fingerprint the persisted bytes, not a copied C++ object:
                    // static_vector copies need not preserve unused slots/padding.
                    std::vector<u8> persisted;
                    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                        fmt::format("{:#018x}", stage_hash), persisted);
                    Serialization::Archive metadata{std::move(persisted)};
                    Serialization::Reader header{metadata};
                    u32 meta_version{}, binary_version{};
                    u64 recorded_stage{}, recorded_permutation{};
                    header.Read(meta_version); header.Read(binary_version);
                    header.Read(recorded_stage); header.Read(recorded_permutation);
                    Shader::StageSpecialization persisted_spec;
                    if (recorded_stage != stage_hash || recorded_permutation != i ||
                        !persisted_spec.Deserialize(metadata))
                        throw std::runtime_error("Shader metadata changed during audit");
                    metadata.Require(sizeof(Shader::InfoPersistent));
                    const std::span<const u8> info_bytes{metadata.CurrPtr(), metadata.Remaining()};
                    std::string descriptors;
                    const auto descriptor = [&](std::string type, size_t count = 1) {
                        if (!descriptors.empty()) descriptors += ',';
                        descriptors += Quote(fmt::format("{}:{}", type, count));
                    };
                    for (const auto& buffer : owned.info->buffers)
                        descriptor(buffer.buffer_type == Shader::BufferType::Flatbuf &&
                                   Shader::UniformConstants(cache.profile, *owned.info)
                                       ? "uniform_buffer" : "storage_buffer");
                    for (const auto& image : owned.info->images)
                        descriptor(image.is_written ? "storage_image" : "sampled_image", image.NumBindings(*owned.info));
                    for (size_t n = 0; n < owned.info->samplers.size(); ++n) descriptor("sampler");
                    Line(out, fmt::format(
                        R"({{"event":"shader_variant","fingerprint_source":"persisted_bytes","binary":{},"program":{},"permutation":{},"stage_id":{},"lookup_equal_to":[{}],"lookup_compared_to":[{}],"comparisons_truncated":{},"binding_start":[{},{},{}],"info_fingerprint":{},"static_info_fingerprint":{},"descriptor_signature":[{}],"resource_counts":[{},{},{},{}]}})",
                        Quote(owned.binary), Quote(fmt::format("{:016x}", program_hash)), i,
                        Quote(fmt::format("{:016x}", stage_hash)), equal, compared, prior.size() > 256,
                        owned.spec.start.unified, owned.spec.start.buffer, owned.spec.start.user_data,
                        Quote(fmt::format("{:016x}", XXH3_64bits(info_bytes.data(), info_bytes.size()))),
                        Quote(fmt::format("{:016x}", XXH3_64bits(info_bytes.data(), sizeof(Shader::InfoPersistent)))), descriptors,
                        owned.info->buffers.size(), owned.info->images.size(),
                        owned.info->samplers.size(), owned.info->fmasks.size()));
                    prior.push_back(std::move(owned));
                }
            }
        }
    };
    static void Reset(PipelineCache& cache, bool empty_native) {
        for (const auto& [_, pipeline] : cache.graphics_pipelines)
            if (pipeline) pipeline->CompleteNative();
        cache.graphics_pipelines.clear();
        cache.compute_pipelines.clear();
        cache.RollbackWarmupModules();
        cache.sel = {.regs = &cache.liverpool->regs};
        if (!cache.program_cache.empty())
            throw std::runtime_error("Replay left shader modules alive between samples");
        if (empty_native || !cache.pipeline_cache) {
            auto [result, native] = cache.instance.GetDevice().createPipelineCacheUnique({});
            if (result != vk::Result::eSuccess)
                throw std::runtime_error("Cannot create empty application pipeline cache");
            cache.pipeline_cache = std::move(native);
        }
    }

    static int Run(PipelineCache& cache, std::ofstream& out, int repeats,
                   const std::string& mode) {
        auto& db = Storage::DataBase::Instance();
        std::vector<u8> bytes;
        db.Load(Storage::BlobType::ShaderProfile, "profile", bytes);
        const bool legacy = !cache.profile.native_constants &&
            bytes.size() == offsetof(Shader::Profile, native_constants);
        Shader::Profile recorded{};
        if (bytes.size() != sizeof(recorded) && !legacy)
            throw std::runtime_error("Missing or incompatible shader profile size");
        std::memcpy(&recorded, bytes.data(), bytes.size());
        if (recorded != cache.profile)
            throw std::runtime_error("Shader profile differs from this GPU/settings; refusing replay");
        // Unlike game warm-up, measurements must have an attributable compiler.
        // Do not rebuild/migrate records or silently bless legacy unstamped shaders.
        db.Load(Storage::BlobType::ShaderCompiler, "translator", bytes = {});
        if (std::string(bytes.begin(), bytes.end()) != Shader::Cache::CompilerKey())
            throw std::runtime_error("Translator stamp missing/stale; rebuild a COPY with bb-shader-lab first");
        cache.require_compiler_stamp = true;
        const auto names = db.ListBlobs(Storage::BlobType::PipelineKey);
        if (names.empty()) throw std::runtime_error("No recorded pipeline keys");
        Line(out, fmt::format(
            R"({{"event":"device","gpu":{},"vendor_id":{},"device_id":{},"driver":{},"compiler":{},"mode":{},"records":{},"dynamic_vertex_input":{},"dynamic_color_write_mask":{}}})",
            Quote(cache.instance.GetModelName()), cache.instance.GetVendorID(),
            cache.instance.GetDeviceID(), cache.instance.GetDriverVersion(),
            Quote(Shader::Cache::CompilerKey()), Quote(mode), names.size(),
            cache.instance.IsVertexInputDynamicState(),
            cache.instance.IsDynamicColorWriteMaskSupported()));
        if (mode == "startup") {
            Reset(cache, true);
            ReplayMetrics::Capture measurement;
            measurement.Start();
            cache.WarmUp(); // Production loader, read-only database, no rebuild/migration.
            measurement.Finish();
            u64 total_ns{};
            for (auto ns : measurement.Nanoseconds()) total_ns += ns;
            const auto progress = PipelineCache::GetPrecacheProgress();
            Line(out, fmt::format(
                R"({{"event":"startup","loaded":{},"rejected":{},"records":{},"total_ms":{:.6f},"metadata_rebuild_skipped":true}})",
                progress.loaded, progress.rejected, progress.total, total_ns / 1e6));
            bool passed = progress.loaded == names.size() && progress.rejected == 0;
            for (const auto& name : names) {
                bool present = false;
                try {
                    db.Load(Storage::BlobType::PipelineKey, name, bytes = {});
                    Serialization::Archive ar{std::move(bytes)};
                    Serialization::Reader reader{ar};
                    u32 version{}, compute{};
                    reader.Read(version); reader.Read(compute);
                    if (version == 6 && compute == 1) {
                        ComputePipelineKey key{}; key.Deserialize(ar);
                        const auto found = cache.compute_pipelines.find(key);
                        present = found != cache.compute_pipelines.end() && found->second && !found->second->NativePending();
                    } else if (version == 6 && compute == 0) {
                        GraphicsPipelineKey key; key.Deserialize(ar);
                        const auto found = cache.graphics_pipelines.find(key);
                        present = found != cache.graphics_pipelines.end() && found->second && !found->second->NativePending();
                    }
                } catch (const std::exception&) {}
                const auto evidence = cache.warmup_evidence.Lookup(name);
                Line(out, fmt::format(
                    R"({{"event":"startup_key","key":{},"ok":{},"warmup":{},"rejection":{}}})",
                    Quote(name), present, Quote(evidence.state), Quote(evidence.rejection)));
                passed &= present;
            }
            // WarmUp retains modules beyond each record. Retire them only after
            // destroying all restored pipelines; no dangling startup snapshot.
            for (const auto& [hash, program] : cache.program_cache)
                for (size_t i = 0; i < program->modules.size(); ++i)
                    if (program->modules[i].module)
                        cache.warmup_modules.push_back({hash, i, program->modules[i].module});
            Reset(cache, false);
            return passed ? 0 : 1;
        }
        int failures = 0;
        VariantAudit variant_audit;
        for (int round = 0; round < repeats; ++round) {
            for (const auto& name : names) {
                Reset(cache, mode == "empty");
                Line(out, fmt::format(R"({{"event":"begin","key":{},"round":{}}})", Quote(name), round));
                auto begin = std::chrono::steady_clock::now();
                ReplayMetrics::Capture measurement;
                bool loaded = false;
                std::string error;
                try {
                    db.Load(Storage::BlobType::PipelineKey, name, bytes = {});
                    Serialization::Archive ar{std::move(bytes)};
                    Serialization::Reader reader{ar};
                    u32 version{}, compute{};
                    reader.Read(version); reader.Read(compute);
                    if (version != 6 || compute > 1)
                        throw std::runtime_error("Replay supports pointer-free v6 pipeline records only");
                    // Audit exactly the same key layout as the production hash.
                    // The projection is diagnostic only: it never changes a replayed key.
                    Serialization::Archive inspect{std::vector<u8>(ar.CurrPtr(), ar.CurrPtr() + ar.Remaining())};
                    std::string canonical, projected, stages, support, expected_name;
                    if (compute) {
                        ComputePipelineKey key{};
                        key.Deserialize(inspect);
                        canonical = fmt::format("{:016x}", key.value);
                        projected = canonical;
                        stages = Quote(canonical);
                        expected_name = fmt::format("c_{:#018x}", key.value);
                    } else {
                        GraphicsPipelineKey key;
                        key.Deserialize(inspect);
                        if (key.num_color_attachments > AmdGpu::NUM_COLOR_BUFFERS)
                            throw std::runtime_error("Invalid color attachment count");
                        canonical = Hex(key.CanonicalBytes());
                        expected_name = fmt::format("g_{:#018x}", std::hash<GraphicsPipelineKey>{}(key));
                        for (auto hash : key.stage_hashes) {
                            if (!stages.empty()) stages += ',';
                            stages += Quote(fmt::format("{:016x}", hash));
                        }
                        if (cache.instance.IsVertexInputDynamicState()) key.vertex_buffer_formats = {};
                        if (cache.instance.IsDynamicColorWriteMaskSupported()) key.write_masks = {};
                        projected = Hex(key.CanonicalBytes());
                    }
                    support = Hex({inspect.CurrPtr(), inspect.Remaining()});
                    if (round == 0) Line(out, fmt::format(
                        R"({{"event":"audit","key":{},"expected_name":{},"kind":{},"canonical":{},"dynamic_projection":{},"stages":[{}],"support":{}}})",
                        Quote(name), Quote(expected_name), Quote(compute ? "compute" : "graphics"), Quote(canonical),
                        Quote(projected), stages, Quote(support)));
                    // Exclude audit/report serialization from measured reconstruction.
                    begin = std::chrono::steady_clock::now();
                    measurement.Start();
                    loaded = compute ? cache.LoadComputePipeline(ar) : cache.LoadGraphicsPipeline(ar, false);
                    if (loaded && !ar.IsEoS()) {
                        loaded = false;
                        error = "Trailing data in pipeline record";
                    }
                    for (const auto& [_, pipeline] : cache.graphics_pipelines)
                        if (pipeline) pipeline->CompleteNative();
                    if (!loaded && error.empty())
                        error = "Production loader rejected metadata, SPIR-V, stage or compiler stamp";
                } catch (const std::exception& e) { error = e.what(); }
                measurement.Finish();
                uint64_t measured_ns{};
                for (const auto ns : measurement.Nanoseconds()) measured_ns += ns;
                // Use the capture's exact stop boundary. A scheduling pause after
                // Finish must not produce a false phase-sum validation failure.
                const double ms = measured_ns ? measured_ns / 1e6 :
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                std::string phases;
                const auto& times = measurement.Nanoseconds();
                for (size_t i = 0; i < times.size(); ++i) {
                    if (i) phases += ',';
                    phases += fmt::format("{}:{:.6f}", Quote(ReplayMetrics::Names[i]), times[i] / 1e6);
                }
                Line(out, fmt::format(
                    R"({{"event":"sample","key":{},"round":{},"ok":{},"reconstruct_ms":{:.6f},"phases_ms":{{{}}},"error":{}}})",
                    Quote(name), round, loaded, ms, phases, Quote(error)));
                if (loaded && round == 0) variant_audit.Record(cache, out);
                failures += !loaded;
            }
        }
        Reset(cache, false);
        return failures ? 1 : 0;
    }
};
}

int main(int argc, char** argv) {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 5) {
        std::fputs("Usage: bb-pipeline-lab CACHE_DIRECTORY REPORT.jsonl REPEATS empty|warm|startup\n", stderr);
        return 2;
    }
    std::ofstream report;
    try {
        const int repeats = std::stoi(argv[3]);
        const std::string mode = argv[4];
        if (repeats < 1 || repeats > 100 || (mode != "empty" && mode != "warm" && mode != "startup") ||
            (mode == "startup" && repeats != 1))
            throw std::runtime_error("Invalid repeats/cache mode");
        // Nothing in the lab may access the player's Vulkan cache or guest process.
        // OfflineReplay bypasses the player's native cache in both cases. The
        // production warm-up entry point additionally needs its enable flag.
        _putenv_s("BB_PIPELINE_CACHE", mode == "startup" ? "1" : "0");
        _putenv_s("BB_PIPELINE_PREP", "0");
        _putenv_s("BB_NULL_GPU", "1");
        auto& db = Storage::DataBase::Instance();
        db.OpenReadOnly(std::filesystem::path(argv[1]));
        const auto output = std::filesystem::weakly_canonical(argv[2]);
        const auto source = std::filesystem::weakly_canonical(argv[1]);
        const auto relative = output.lexically_relative(source);
        if (relative.empty() || *relative.begin() != "..")
            throw std::runtime_error("Report must be outside the input cache");
        report.open(output, std::ios::out | std::ios::trunc);
        if (!report) throw std::runtime_error("Cannot open report");
        Vulkan::Instance instance{EmulatorSettings.GetGpuId(), false};
        Vulkan::Scheduler scheduler{instance, false};
        AmdGpu::Liverpool gpu;
        // No buffers are allocated/dispatched. The recorded sparse-page size is a
        // shader ABI input; all actual device feature fields are still checked.
        std::vector<u8> profile;
        db.Load(Storage::BlobType::ShaderProfile, "profile", profile);
        u32 shift = 16;
        if (profile.size() >= offsetof(Shader::Profile, sparse_page_shift) + sizeof(shift))
            std::memcpy(&shift, profile.data() + offsetof(Shader::Profile, sparse_page_shift), sizeof(shift));
        if (shift < 12 || shift > 30) throw std::runtime_error("Invalid sparse page size");
        Vulkan::PipelineCache cache{instance, scheduler, &gpu, shift,
                                    Vulkan::PipelineCache::Mode::OfflineReplay};
        const int result = Vulkan::PipelineReplay::Run(cache, report, repeats, mode);
        db.Close();
        return result;
    } catch (const std::exception& e) {
        // An unopened ofstream still has goodbit set. Reporting a pre-open error
        // must not itself throw and turn a clean rejection into an abort.
        if (report.is_open()) {
            try {
                Line(report, fmt::format(R"({{"event":"fatal","error":{}}})", Quote(e.what())));
            } catch (const std::exception&) {}
        }
        std::fprintf(stderr, "Pipeline replay failed: %s\n", e.what());
        Storage::DataBase::Instance().Close();
        return 2;
    }
}
