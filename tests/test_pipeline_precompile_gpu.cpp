// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/hash.h"
#include "common/serdes.h"
#include "video_core/cache_storage.h"
#include "pipeline_prep_comp.h"
#include "pipeline_prep_frag.h"
#include "pipeline_prep_quad.h"
#include "pipeline_prep_vert.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/preparation_shader_params.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include <array>
#include <cassert>
#include <chrono>
#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <windows.h>

namespace AmdGpu {
struct LiverpoolTaskTest {
    static void SelectGraphicsQueue(Liverpool& gpu) { gpu.curr_qid = Liverpool::GfxQueueId; }
};
}

namespace Vulkan {
struct PipelinePrecompileTest {
    static void OfflineReplayGuards() {
        const auto root = std::filesystem::path{"out/test-fixtures/pipeline-replay-native"};
        std::filesystem::create_directories(root);
        const auto file = root / "profile.bin";
        { std::ofstream stream(file, std::ios::binary); stream << "unchanged"; }
        auto& db = Storage::DataBase::Instance();
        db.OpenReadOnly(root);
        // Offline loading must not accumulate executable SRT walkers per repeat.
        std::array<u8, 4> code{0x90, 0x90, 0x90, 0xc3};
        Shader::PersistentSrtInfo original{};
        original.walker_func = reinterpret_cast<Shader::PFN_SrtWalker>(code.data());
        original.walker_func_size = code.size();
        Serialization::Archive writer;
        original.Serialize(writer);
        Serialization::Archive reader{writer.TakeOff()};
        Shader::PersistentSrtInfo restored{};
        assert(restored.Deserialize(reader));
        assert(reader.IsEoS() && !restored.walker_func && restored.walker_func_size == 0);
        assert(!db.Save(Storage::BlobType::ShaderProfile, "profile", std::vector<u8>{1, 2}));
        assert(!db.Save(Storage::BlobType::ShaderBinary, "unused", std::vector<u32>{1, 2}));
        db.Clear();
        db.FinishPreload();
        std::vector<u8> bytes;
        db.Load(Storage::BlobType::ShaderProfile, "profile", bytes);
        assert(std::string(bytes.begin(), bytes.end()) == "unchanged");
        db.Close();
        assert(!std::filesystem::exists(root / "unused.spv"));
        puts("PASS: replay database refuses writes/clear; offline metadata skips executable walkers");
    }
    static void ClipPermutationReuse(PipelineCache& cache, AmdGpu::Liverpool& gpu) {
        const auto saved = std::make_unique<AmdGpu::Regs>(gpu.regs);
        const bool clip_profile = cache.profile.needs_clip_distance_emulation;
        cache.profile.needs_clip_distance_emulation = true;
        gpu.regs = {};
        gpu.regs.primitive_type = AmdGpu::PrimitiveType::TriangleList;
        gpu.regs.clipper_control.user_clip_plane_enable = 1;
        gpu.regs.num_interp = 6;
        for (u32 i = 0; i < gpu.regs.ps_inputs.size(); ++i) gpu.regs.ps_inputs[i].input_offset = i;
        const std::array<u32, 3> code{0xBEEB03FF, 0, 0xBF810000};
        const std::array<u32, 16> user_data{};
        constexpr u64 hash = 0x880010;
        const Shader::ShaderParams params{user_data, code, hash};
        PipelineSelection selection{};
        selection.regs = &gpu.regs;
        const auto select = [&] {
            Shader::Backend::Bindings bindings{};
            selection.cold_shader = selection.new_permutation = false;
            return cache.GetProgram(selection, Shader::HwStage::Fragment,
                                    Shader::SwStage::Fragment, params, bindings);
        };
        const auto first = select();
        auto& program = *cache.program_cache.at(hash);
        for (unsigned i = 0; i < 40; ++i) {
            const auto next = select();
            assert(std::get<1>(next) == std::get<1>(first));
            assert(std::get<3>(next) == std::get<3>(first));
            assert(!selection.new_permutation && program.modules.size() == 1);
        }
        // A real change in fragment input layout must still get its own shader.
        gpu.regs.num_interp = 7;
        const auto changed = select();
        assert(selection.new_permutation && program.modules.size() == 2);
        assert(std::get<3>(changed) != std::get<3>(first));
        for (unsigned i = 0; i < 20; ++i) {
            assert(std::get<1>(select()) == std::get<1>(changed));
            assert(!selection.new_permutation && program.modules.size() == 2);
        }
        gpu.regs.num_interp = 6;
        assert(std::get<1>(select()) == std::get<1>(first));
        assert(!selection.new_permutation && program.modules.size() == 2);
        cache.profile.needs_clip_distance_emulation = clip_profile;
        gpu.regs = *saved;
        puts("PASS: production fragment lookup reuses clip-distance shaders across repeated draws; changed input layouts remain distinct");
    }

    static void WarmupRollback(PipelineCache& cache, vk::ShaderModule stable) {
        constexpr u64 good_hash=0xabcd0001, bad_hash=0xabcd0002;
        auto good=std::make_unique<Program>(); good->info.pgm_hash=good_hash;
        good->AddPermut(stable,{});
        cache.program_cache.emplace(good_hash,std::move(good));
        const auto temporary=CompileSPV(PIPELINE_PREP_COMP,cache.instance.GetDevice());
        auto bad=std::make_unique<Program>(); bad->info.pgm_hash=bad_hash;
        bad->AddPermut(temporary,{});
        cache.program_cache.emplace(bad_hash,std::move(bad));
        cache.warmup_modules.push_back({bad_hash,0,temporary});
        cache.RollbackWarmupModules();
        assert(!cache.program_cache.contains(bad_hash));
        assert(cache.program_cache.at(good_hash)->modules[0].module==stable);
        cache.program_cache.erase(good_hash);
        puts("PASS: rejected warm-up rolls back its new modules while retaining compatible shaders");
    }
    static void RolledBackPermutation(PipelineCache& cache, AmdGpu::Liverpool& gpu) {
        auto& regs = gpu.GetCsRegs();
        const auto saved = regs;
        regs = {};
        regs.num_thread_x.full = regs.num_thread_y.full = regs.num_thread_z.full = 1;
        const std::array<u32, 3> code{0xBEEB03FF, 0, 0xBF810000};
        const std::array<u32, 16> user_data{};
        constexpr u64 hash = 0x880011;
        const Shader::ShaderParams params{user_data, code, hash};
        PipelineSelection selection{};
        selection.regs = &gpu.regs;
        selection.compute_regs = &regs;
        const auto select = [&](PrepWorker* worker = nullptr) {
            selection.worker = worker;
            selection.cold_shader = selection.new_permutation = false;
            Shader::Backend::Bindings bindings{};
            return cache.GetProgram(selection, Shader::HwStage::Compute,
                                    Shader::SwStage::Compute, params, bindings);
        };
        const auto lower = select();
        regs.num_thread_x.full = 2;
        const auto higher = select();
        auto& program = *cache.program_cache.at(hash);
        assert(program.modules.size() == 2 && std::get<1>(lower) && std::get<1>(higher));
        const auto old_spec = program.modules[0].spec;
        cache.warmup_modules.push_back({hash, 0, std::get<1>(lower)});
        cache.RollbackWarmupModules();
        assert(program.modules.size() == 2 && !program.modules[0].module);
        assert(program.modules[1].module == std::get<1>(higher));

        regs.num_thread_x.full = 1;
        PrepWorker missing;
        const auto absent = select(&missing);
        assert(!std::get<1>(absent) && missing.failed && missing.missing_shader);
        assert(!program.modules[0].spec.Valid());

        // Also reject an otherwise matching specialization with no module. This
        // covers both lookup paths independently of rollback invalidation.
        program.modules[0].spec = old_spec;
        PrepWorker stale;
        assert(!std::get<1>(select(&stale)) && stale.failed && stale.missing_shader);
        program.last_used = 0;
        const auto rebuilt = select();
        assert(selection.new_permutation && std::get<1>(rebuilt));
        assert(program.modules.size() == 3 && std::get<3>(rebuilt) == HashCombine(hash, 2));
        // The stale last-used slot and the full search must both skip the hole.
        assert(std::get<1>(select()) == std::get<1>(rebuilt) && !selection.new_permutation);
        PrepWorker ready;
        assert(std::get<1>(select(&ready)) == std::get<1>(rebuilt) && !ready.failed);
        regs.num_thread_x.full = 2;
        const auto retained = select();
        assert(std::get<1>(retained) == std::get<1>(higher));
        assert(std::get<3>(retained) == HashCombine(hash, 1) && program.modules.size() == 3);
        program.modules[0].spec = {};

        ComputePipeline::SerializationSupport data;
        ComputePipeline pipeline{cache.instance, cache.scheduler, cache.desc_heap, cache.profile,
            *cache.pipeline_cache, {std::get<3>(rebuilt)}, *std::get<0>(rebuilt),
            std::get<1>(rebuilt), data, false};
        assert(pipeline.Handle());
        regs = saved;
        puts("PASS: partial rollback preserves higher indices, both lookups reject dead modules, "
             "required lookup recompiles and creates a native pipeline");
    }
    static void MissingModules(PipelineCache& cache, PipelineSelection& selection) {
        for (const bool preloading : {false, true}) {
            for (const auto stage : {Shader::SwStage::Vertex, Shader::SwStage::Fragment}) {
                auto modules = selection.modules;
                modules[u32(stage)] = nullptr;
                bool rejected = false, scheduled = false;
                GraphicsPipeline::SerializationSupport data;
                try {
                    GraphicsPipeline pipeline{cache.instance, cache.scheduler, cache.desc_heap,
                        cache.profile, selection.graphics_key, *cache.pipeline_cache,
                        selection.infos, selection.runtime_infos, selection.fetch_shader,
                        modules, data, preloading, !preloading,
                        [&](Pipeline::NativeWork work) {
                            scheduled = true;
                            return work;
                        }};
                } catch (const std::invalid_argument& error) {
                    rejected = std::string_view{error.what()}.find("missing shader module") !=
                               std::string_view::npos;
                }
                assert(rejected && !scheduled);
            }
        }
        bool rejected = false;
        GraphicsPipeline::SerializationSupport graphics_data;
        try {
            GraphicsPipeline pipeline{cache.instance, cache.scheduler, cache.desc_heap,
                cache.profile, selection.graphics_key, *cache.pipeline_cache,
                selection.infos, selection.runtime_infos, selection.fetch_shader,
                std::span<const vk::ShaderModule>{}, graphics_data, false};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
        for (const bool preloading : {false, true}) {
            rejected = false;
            ComputePipeline::SerializationSupport data;
            Shader::Info info;
            try {
                ComputePipeline pipeline{cache.instance, cache.scheduler, cache.desc_heap,
                    cache.profile, *cache.pipeline_cache, {0xdeadbeef}, info, {}, data,
                    preloading, !preloading};
            } catch (const std::invalid_argument& error) {
                const std::string_view message{error.what()};
                rejected = message.find("missing shader module") != message.npos &&
                           message.find("0xdeadbeef") != message.npos;
            }
            assert(rejected);
        }
        puts("PASS: missing graphics/compute modules and truncated stage arrays are rejected "
             "before native creation or scheduling");
    }
    static void RequiredEntry(PipelineCache& cache, AmdGpu::Liverpool& gpu, vk::ShaderModule module, bool lookahead = false) {
        auto saved=std::make_unique<AmdGpu::Regs>(gpu.regs);
        gpu.regs={};gpu.regs.primitive_type=AmdGpu::PrimitiveType::TriangleList;
        alignas(256) std::array<u32,128> code{};
        code[0]=0xBEEB03FF;code[1]=1;
        AmdGpu::BinaryInfo binary{};binary.signature=AmdGpu::BinaryInfo::signature_ref;
        binary.length=8;binary.shader_hash=lookahead ? 0x880002 : 0x880001;
        std::memcpy(code.data()+4,&binary,sizeof(binary));
        gpu.regs.vs_program.address=reinterpret_cast<uintptr_t>(code.data())>>8;
        const auto params=AmdGpu::GetParams(gpu.regs.vs_program);
        auto program=std::make_unique<Program>(Shader::HwStage::Vertex,Shader::SwStage::Vertex,params);
        program->info.RefreshFlatBuf();
        PipelineSelection selection{};selection.regs=&gpu.regs;
        const auto runtime=cache.BuildRuntimeInfo(selection,Shader::HwStage::Vertex,Shader::SwStage::Vertex);
        Shader::Backend::Bindings binding{};
        program->AddPermut(module,Shader::StageSpecialization(program->info,runtime,cache.profile,binding));
        program->info_template=std::make_unique<Shader::Info>(program->info);
        { std::unique_lock lock{cache.programs_mutex};cache.program_cache.emplace(params.hash,std::move(program)); }
        cache.ProgramPublished();
        const auto before=cache.precompiler->RequiredQueued();
        if(lookahead) {
            PrepWorker worker;selection.worker=&worker;
            assert(cache.PrepareGraphicsPipeline(selection));
        }
        const auto* pipeline=cache.GetGraphicsPipeline();
        const bool enabled=!(std::getenv("BB_FIRST_USE_PIPELINE") && std::getenv("BB_FIRST_USE_PIPELINE")[0]=='0');
        assert(pipeline && pipeline->NativePending()==(lookahead ? cache.early_frontend : enabled));
        assert(cache.precompiler->RequiredQueued()==before+unsigned(!lookahead));
        assert(cache.GetGraphicsPipeline()==pipeline); // Pending metadata is safely cacheable.
        assert(pipeline->Handle() && !pipeline->NativePending());
        gpu.regs=*saved;
        puts("PASS: production GetGraphicsPipeline queues first-use native, caches frontend, waits at handle; legacy toggle is honoured");
    }
    static auto Early(PipelineCache& cache, PipelineSelection& selection) {
        // Gate the real production continuation after metadata publication.
        // This draw has to expose layout/resources before native creation runs.
        auto key=selection.graphics_key;key.motion_vectors=0;key.stage_hashes[0]^=0x80000000u;
        auto owned=selection;owned.graphics_key=key;
        std::promise<void> release;auto gate=release.get_future().share();
        assert(cache.precompiler->QueueEarly(key,[&] {
            auto work=cache.MakeEarlyGraphicsWork(owned);
            return [work=std::move(work),gate](auto publish,auto timing) {
                work([publish,gate](PipelineCache::PreparedPipeline result) {
                    publish(std::move(result));gate.wait();
                },timing);
            };
        }));
        PipelineTiming timing;
        auto result=cache.precompiler->TakeOrReserve(key,&timing);
        assert(result && result->pipeline && result->pipeline->NativePending());
        auto pipeline=std::move(result->pipeline);
        pipeline->BindRuntimeStages(selection.infos);result->snapshot.reset();
        assert(pipeline->GetLayout());
        auto handle=std::async(std::launch::async,[&] { return pipeline->Handle(); });
        assert(handle.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
        release.set_value();assert(handle.get() && !pipeline->NativePending());
        puts("PASS: real lookahead frontend consumed before native continuation, bind waits, owned state survives snapshot release");
        return pipeline;
    }
    static auto Deferred(PipelineCache& cache, PipelineSelection& selection) {
        // Keep the sole compiler occupied. Frontend construction must still
        // return, exposing the layout/resources while the native handle waits.
        GraphicsPipelineKey blocker{}; blocker.stage_hashes[0]=0xfedcba+u32(selection.graphics_key.prim_type);
        std::promise<void> started,release;
        auto gate=release.get_future().share();
        assert(cache.precompiler->Queue(blocker,[&] { return [&] {
            started.set_value();gate.wait();return PipelineCache::PreparedPipeline{};
        }; }));
        started.get_future().wait();
        GraphicsPipeline::SerializationSupport data;
        auto pipeline=std::make_unique<GraphicsPipeline>(cache.instance,cache.scheduler,cache.desc_heap,
            cache.profile,selection.graphics_key,*cache.preparation_cache,selection.infos,
            selection.runtime_infos,selection.fetch_shader,selection.modules,data,false,false,
            cache.FirstUseSchedule(selection.graphics_key));
        assert(pipeline->NativePending() && pipeline->GetLayout());
        // Invalidate caller-owned serialization arrays before the driver reads.
        data={};
        release.set_value();
        assert(cache.precompiler->TakeOrReserve(blocker));
        return pipeline;
    }
    static void ScanGraphics(PipelineCache& cache, vk::ShaderModule module, bool enabled = true, bool late = false, bool spread = true) {
        alignas(256) std::array<u32,128> code{};
        code[0]=0xBEEB03FF;code[1]=1;
        AmdGpu::BinaryInfo binary{};binary.signature=AmdGpu::BinaryInfo::signature_ref;
        binary.length=8;binary.shader_hash=(enabled ? 0x987654 : 0x987655)+(late?2:0)+(spread?4:0);
        std::memcpy(code.data()+4,&binary,sizeof(binary));
        auto regs=std::make_unique<AmdGpu::Regs>();
        regs->primitive_type=AmdGpu::PrimitiveType::TriangleList;
        regs->vs_program.address=reinterpret_cast<uintptr_t>(code.data())>>8;
        const auto params=AmdGpu::GetParams(regs->vs_program);
        auto program=std::make_unique<Program>(Shader::HwStage::Vertex,Shader::SwStage::Vertex,params);
        program->info.RefreshFlatBuf();
        PipelineSelection selection{};selection.regs=regs.get();
        const auto runtime=cache.BuildRuntimeInfo(selection,Shader::HwStage::Vertex,Shader::SwStage::Vertex);
        Shader::Backend::Bindings binding{};
        program->AddPermut(module,Shader::StageSpecialization(program->info,runtime,cache.profile,binding));
        program->info_template=std::make_unique<Shader::Info>(program->info);
        { std::unique_lock lock{cache.programs_mutex};cache.program_cache.emplace(params.hash,std::move(program)); }
        cache.ProgramPublished();
        _putenv_s("BB_PREP_WORKERS","1");_putenv_s("BB_GRAPHICS_SCAN",enabled ? "1" : "0");
        _putenv_s("BB_GRAPHICS_SCAN_SPREAD",spread ? "1" : "0");
        DrawPreparation preparation{cache};
        AmdGpu::PM4Type3Header type3{AmdGpu::PM4ItOpcode::DrawIndexAuto,1};
        u32 header;std::memcpy(&header,&type3,4);
        std::vector<u32> commands;
        for(unsigned i=0;i<17;++i) {
            if(late && i==16) {
                AmdGpu::PM4Type3Header set{AmdGpu::PM4ItOpcode::SetUconfigReg,1};
                u32 change;std::memcpy(&change,&set,4);
                constexpr u32 offset=offsetof(AmdGpu::Regs,primitive_type)/4-AmdGpu::Regs::UconfigRegWordOffset;
                commands.insert(commands.end(),{change,offset,u32(AmdGpu::PrimitiveType::TriangleStrip)});
            }
            commands.insert(commands.end(),{header,3,0});
        }
        const auto before=cache.precompiler->Queued();
        auto submission=preparation.Build(commands);
        submission->retry.Claim(cache.ProgramGeneration()); // Isolate scanner coverage from draw workers.
        preparation.Enqueue(0,submission);
        preparation.BeginSubmission(0,*regs,0);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!submission->scanned.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        assert(submission->scanned.load(std::memory_order_acquire));
        assert(preparation.graphics_scan_attempts.load()==(enabled ? 16u : 0u) &&
               preparation.graphics_scan_ready.load()==(enabled ? 16u : 0u));
        if (enabled) assert(cache.precompiler->Queued()==before+(late && spread ? 2u : 1u));
        PrepWorker worker;selection.worker=&worker;
        assert(cache.PrepareGraphicsPipeline(selection));
        auto ready=cache.precompiler->TakeOrReserve(selection.graphics_key);
        assert(ready && ready->pipeline);
        preparation.EndSubmission();
        puts("PASS: scanner queues known graphics before publication, bounds attempts, deduplicates and compiles owned selection");
    }
    static void ScanCompute(PipelineCache& cache) {
        _putenv_s("BB_PREP_WORKERS", "1");
        _putenv_s("BB_COMPUTE_SCAN", "1");
        DrawPreparation preparation{cache};
        AmdGpu::PM4Type3Header type3{AmdGpu::PM4ItOpcode::DispatchDirect, 3};
        u32 header; std::memcpy(&header, &type3, 4);
        const std::array<u32,15> commands{header,0,1,1,1, header,2,1,1,0, header,2,1,1,1};
        auto submission=preparation.Build(commands);
        assert(submission);
        preparation.Enqueue(0,submission);
        auto regs=std::make_unique<AmdGpu::Regs>();
        preparation.BeginSubmission(0,*regs,0);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!submission->scanned.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        assert(submission->scanned.load(std::memory_order_acquire));
        assert(preparation.compute_scan_attempts.load()==1 && preparation.compute_scan_ready.load()==1);
        assert(cache.compute_precompiler->Queued()==1 && cache.compute_precompiler->RequiredQueued()==0);
        preparation.EndSubmission();
        puts("PASS: real scanner queues compute before publishing draws, ignores empty/disabled dispatches");
    }
    static auto Consume(PipelineCache &cache, PipelineSelection &selection, bool required = false) {
        assert(cache.precompiler);
        std::optional<PipelineCache::PreparedPipeline> ready;
        if (required) {
            const auto before = cache.precompiler->RequiredQueued();
            ready = cache.precompiler->TakeOrQueue(
                selection.graphics_key, [&] { return cache.MakeGraphicsWork(selection); });
            assert(cache.precompiler->RequiredQueued() == before + 1);
        } else {
            cache.QueuePreparedPipeline(selection);
            const_cast<u32 *>(selection.infos[u32(Shader::SwStage::Vertex)]->user_data.data())[0] =
                999;
            ready = cache.precompiler->TakeOrReserve(selection.graphics_key);
        }
        assert(ready && ready->pipeline);
        assert(ready->snapshot->infos[u32(Shader::SwStage::Vertex)]->user_data[0] == 123);
        ready->pipeline->BindRuntimeStages(selection.infos);
        ready->snapshot.reset(); // Runtime stage pointers must not retain the freed snapshot.
        assert(&ready->pipeline->GetStage(Shader::SwStage::Vertex) ==
               selection.infos[u32(Shader::SwStage::Vertex)]);
        return std::move(ready->pipeline);
    }
    static const Shader::Info *InstallCompute(PipelineCache &cache,
                                             const AmdGpu::ComputeProgram &regs,
                                             vk::ShaderModule module, bool template_ready = true) {
        const auto params = AmdGpu::GetParams(regs);
        auto program =
            std::make_unique<Program>(Shader::HwStage::Compute, Shader::SwStage::Compute, params);
        program->info.srt_info.flattened_bufsize_dw = regs.user_data.size();
        program->info.buffers.push_back(Shader::BufferResource{.is_written = true});
        program->info.RefreshFlatBuf();
        PipelineSelection selection{};
        selection.compute_regs = &regs;
        const auto runtime =
            cache.BuildRuntimeInfo(selection, Shader::HwStage::Compute, Shader::SwStage::Compute);
        Shader::Backend::Bindings binding{};
        program->AddPermut(
            module, Shader::StageSpecialization(program->info, runtime, cache.profile, binding));
        if (template_ready)
            program->info_template = std::make_unique<Shader::Info>(program->info);
        auto *info = &program->info;
        {
            std::unique_lock lock{cache.programs_mutex};
            cache.program_cache.emplace(params.hash, std::move(program));
        }
        cache.ProgramPublished();
        return info;
    }
    static void CheckComputeAdoption(PipelineCache &cache, bool prepared) {
        const bool deferred = !(std::getenv("BB_FIRST_USE_COMPUTE") && std::getenv("BB_FIRST_USE_COMPUTE")[0] == '0');
        assert(cache.prepared_compute_hits == (prepared || deferred ? 1u : 2u));
        assert(cache.compute_precompiler->RequiredQueued() == (prepared ? 0u : 1u));
    }
    static std::function<void()> GateCompute(PipelineCache& cache) {
        static u64 next{};
        const ComputePipelineKey key{0xf00d0000 + next++};
        auto release = std::make_shared<std::promise<void>>();
        auto gate = release->get_future().share();
        auto entered = std::make_shared<std::promise<void>>();
        auto started = entered->get_future();
        assert(cache.compute_precompiler->Queue(key, [gate, entered] {
            return [gate, entered] { entered->set_value(); gate.wait(); return PipelineCache::PreparedCompute{}; };
        }));
        assert(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        return [release, &cache, key] {
            release->set_value();
            assert(cache.compute_precompiler->TakeOrReserve(key));
        };
    }
    static void ComputeDrain(PipelineCache& cache) {
        auto release = GateCompute(cache);
        const ComputePipelineKey key{0xfeed0002};
        ComputePipeline::SerializationSupport data;
        auto pipeline = std::make_unique<ComputePipeline>(cache.instance, cache.scheduler,
            cache.desc_heap, cache.profile, *cache.compute_preparation_cache, key,
            *cache.sel.infos[0], cache.sel.modules[0], data, false, false,
            cache.FirstUseComputeSchedule(key));
        assert(pipeline->NativePending() && pipeline->GetLayout());
        cache.compute_pipelines.emplace(key, std::move(pipeline));
        auto drain = std::async(std::launch::async, [&] { cache.Sync(); });
        assert(drain.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
        release();
        drain.get();
        assert(!cache.compute_pipelines.at(key)->NativePending());
        puts("PASS: shutdown drains an unused pending compute pipeline before merging/stopping its cache");
    }
    static void CheckPriority(PipelineCache& cache) {
        std::promise<int> native_priority;
        auto observed = native_priority.get_future();
        GraphicsPipelineKey first_use{};
        first_use.stage_hashes[0] = 0x770011;
        auto completion = cache.FirstUseSchedule(first_use)([&] {
            native_priority.set_value(GetThreadPriority(GetCurrentThread()));
            return vk::UniquePipeline{};
        });
        assert(observed.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        const bool boosted = !(std::getenv("BB_REQUIRED_PIPELINE_PRIORITY") &&
                               std::getenv("BB_REQUIRED_PIPELINE_PRIORITY")[0] == '0');
        assert(observed.get() == (boosted ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_IDLE));
        completion(); // Deliberately consume only after observing the worker's priority.
        std::promise<int> idle_priority;
        auto idle = idle_priority.get_future();
        GraphicsPipelineKey speculative{};
        speculative.stage_hashes[0] = 0x770012;
        assert(cache.precompiler->Queue(speculative, [&] { return [&] {
            idle_priority.set_value(GetThreadPriority(GetCurrentThread()));
            return PipelineCache::PreparedPipeline{};
        }; }));
        assert(idle.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        assert(idle.get() == THREAD_PRIORITY_IDLE);
        assert(cache.precompiler->TakeOrReserve(speculative));
        puts("PASS: required native work gets priority before any bind wait; speculative work restores idle priority");
        GraphicsPipelineKey key{};
        key.prim_type = AmdGpu::PrimitiveType::TriangleFan;
        auto result = cache.precompiler->TakeOrQueue(key, [] {
            return [] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (GetThreadPriority(GetCurrentThread()) != THREAD_PRIORITY_NORMAL &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                assert(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL);
                return PipelineCache::PreparedPipeline{};
            };
        });
        assert(result);
        puts("PASS: Windows compiler priority rises to normal while its GPU consumer waits");
    }
    static auto Inline(PipelineCache &cache, PipelineSelection &selection) {
        GraphicsPipeline::SerializationSupport data;
        return std::make_unique<GraphicsPipeline>(
            cache.instance, cache.scheduler, cache.desc_heap, cache.profile, selection.graphics_key,
            *cache.pipeline_cache, selection.infos, selection.runtime_infos, selection.fetch_shader,
            selection.modules, data, false);
    }
};
} // namespace Vulkan

void CheckRegisterReplay() {
    using namespace AmdGpu;
    auto shadow = std::make_unique<Regs>();
    auto reference = std::make_unique<Regs>();
    auto replay = std::make_unique<Regs>();
    u64 checksum = 0, expected = 0;
    const auto header = [](PM4ItOpcode opcode, u32 payload) {
        PM4Type3Header type3{opcode, payload - 1};
        u32 result;
        std::memcpy(&result, &type3, 4);
        return result;
    };
    const std::array<u32, 4> set{header(PM4ItOpcode::SetShReg, 3), 0x200, 9, 7};
    RegDirty dirty;
    auto *packet = reinterpret_cast<const PM4Header *>(set.data());
    Vulkan::DrawPreparation::ApplyPreparationRegisters(*shadow, packet, checksum, &dirty);
    Liverpool::ApplyGraphicsRegisterPacket(*reference, packet, expected);
    assert(checksum == expected && shadow->cs_program.dispatch_initiator == 9 &&
           shadow->cs_program.dim_x == 7);
    RegDelta delta;
    delta.Capture(*shadow, dirty);
    delta.Apply(*replay);
    assert(std::memcmp(&shadow->cs_program, &replay->cs_program, sizeof(ComputeProgram)) == 0);
    const auto saved = shadow->cs_program;
    const std::array<u32, 2> clear{header(PM4ItOpcode::ClearState, 1), 0};
    packet = reinterpret_cast<const PM4Header *>(clear.data());
    Vulkan::DrawPreparation::ApplyPreparationRegisters(*shadow, packet, checksum, &dirty);
    Liverpool::ApplyGraphicsRegisterPacket(*reference, packet, expected);
    assert(checksum == expected && std::memcmp(&shadow->cs_program, &saved, sizeof(saved)) == 0);
    delta.Capture(*shadow, dirty);
    delta.Apply(*replay);
    assert(std::memcmp(&shadow->cs_program, &replay->cs_program, sizeof(saved)) == 0);
    const std::array<u32, 5> dispatch{header(PM4ItOpcode::DispatchDirect, 4), 2, 3, 4, 1};
    packet = reinterpret_cast<const PM4Header *>(dispatch.data());
    Vulkan::DrawPreparation::ApplyPreparationRegisters(*shadow, packet, checksum);
    assert(shadow->cs_program.dim_x == 2 && shadow->cs_program.dim_y == 3 &&
           shadow->cs_program.dim_z == 4 && shadow->cs_program.dispatch_initiator == 1);
    puts("PASS: compute shadow replays partial SH writes, scanner deltas, "
         "CLEAR_STATE and direct dispatch dimensions without changing graphics "
         "checksums");
    alignas(256) std::array<u32, 0x4000> absent_shader{};
    ComputeProgram unknown{};
    assert(!Vulkan::TryPreparationParams(unknown));
    unknown.address = reinterpret_cast<uintptr_t>(absent_shader.data()) >> 8;
    absent_shader[0] = 0xBEEB03FF;
    absent_shader[1] = 0xffffffff;
    assert(!Vulkan::TryPreparationParams(unknown));
    puts("PASS: absent and incomplete speculative shader headers return "
         "unavailable without terminating the game");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _putenv_s("BB_PIPELINE_CACHE",
              "0"); // Do not read or modify the player's shader cache.
    _putenv_s("BB_PIPELINE_PREP", "1");
    _putenv_s("BB_NULL_GPU", "1");
    CheckRegisterReplay();
    Vulkan::PipelinePrecompileTest::OfflineReplayGuards();
    using namespace Vulkan;
    Instance instance{-1, false};
    if (!instance.IsVertexInputDynamicState()) {
        puts("SKIP: background pipeline preparation requires dynamic vertex input");
        return 0;
    }
    Scheduler scheduler{instance, false};
    AmdGpu::Liverpool gpu;
    AmdGpu::LiverpoolTaskTest::SelectGraphicsQueue(gpu);
    const auto device = instance.GetDevice();
    // Explicit entry points avoid conflicting Vulkan-Hpp inline init definitions
    // between asserted fixture headers and production platform headers.
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    const auto get_instance =
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    const auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance(static_cast<VkInstance>(instance.GetInstance()), "vkGetDeviceProcAddr"));
#define LOAD(name)                                                                                 \
    dispatch.name = reinterpret_cast<PFN_##name>(get_device(static_cast<VkDevice>(device), #name))
    LOAD(vkCreateShaderModule);
    LOAD(vkDestroyShaderModule);
    LOAD(vkCreateImageView);
    LOAD(vkDestroyImageView);
    LOAD(vkCmdBindPipeline);
    LOAD(vkCmdBeginRendering);
    LOAD(vkCmdEndRendering);
    LOAD(vkCmdDraw);
    LOAD(vkCmdSetVertexInputEXT);
    LOAD(vkCmdPipelineBarrier2);
    LOAD(vkCmdCopyImageToBuffer);
    LOAD(vkCmdDispatch);
    LOAD(vkCmdPushDescriptorSetKHR);
#undef LOAD
    auto vertex = device
                      .createShaderModuleUnique(
                          {.codeSize = sizeof(PIPELINE_PREP_VERT), .pCode = PIPELINE_PREP_VERT},
                          nullptr, dispatch)
                      .value;
    auto fragment = device
                        .createShaderModuleUnique(
                            {.codeSize = sizeof(PIPELINE_PREP_FRAG), .pCode = PIPELINE_PREP_FRAG},
                            nullptr, dispatch)
                        .value;
    auto quad = device
                    .createShaderModuleUnique(
                        {.codeSize = sizeof(PIPELINE_PREP_QUAD), .pCode = PIPELINE_PREP_QUAD},
                        nullptr, dispatch)
                    .value;
    auto compute = device
                       .createShaderModuleUnique(
                           {.codeSize = sizeof(PIPELINE_PREP_COMP), .pCode = PIPELINE_PREP_COMP},
                           nullptr, dispatch)
                       .value;
    PipelineCache cache{instance, scheduler, &gpu, 16};
    PipelinePrecompileTest::ClipPermutationReuse(cache, gpu);
    PipelinePrecompileTest::WarmupRollback(cache,*compute);
    PipelinePrecompileTest::RolledBackPermutation(cache, gpu);
    PipelinePrecompileTest::CheckPriority(cache);
    PipelinePrecompileTest::ScanGraphics(cache,*vertex);
    PipelinePrecompileTest::ScanGraphics(cache,*vertex,false);
    PipelinePrecompileTest::ScanGraphics(cache,*vertex,true,true,true);
    PipelinePrecompileTest::ScanGraphics(cache,*vertex,true,true,false);
    PipelinePrecompileTest::RequiredEntry(cache,gpu,*vertex);
    PipelinePrecompileTest::RequiredEntry(cache,gpu,*vertex,true);
    Shader::Info vs{}, fs{};
    vs.hw_stage = Shader::HwStage::Vertex;
    vs.sw_stage = Shader::SwStage::Vertex;
    vs.pgm_hash = 1;
    fs.hw_stage = Shader::HwStage::Fragment;
    fs.sw_stage = Shader::SwStage::Fragment;
    fs.pgm_hash = 2;
    std::array<u32, 1> ud{123};
    vs.user_data = ud;
    vs.flattened_ud_buf = {123};
    PipelineSelection selection{};
    selection.infos[u32(Shader::SwStage::Vertex)] = &vs;
    selection.infos[u32(Shader::SwStage::Fragment)] = &fs;
    selection.modules[u32(Shader::SwStage::Vertex)] = *vertex;
    selection.modules[u32(Shader::SwStage::Fragment)] = *fragment;
    auto &key = selection.graphics_key;
    key.prim_type = AmdGpu::PrimitiveType::TriangleList;
    key.logic_op = AmdGpu::ColorControl::LogicOp::Copy;
    key.polygon_mode = AmdGpu::PolygonMode::Fill;
    key.num_samples = key.depth_samples = key.color_samples[0] = 1;
    key.num_color_attachments = 1;
    key.stage_hashes[u32(Shader::SwStage::Vertex)] = 1;
    key.stage_hashes[u32(Shader::SwStage::Fragment)] = 2;
    key.color_buffers[0].data_format = AmdGpu::DataFormat::Format8_8_8_8;
    key.color_buffers[0].num_format = AmdGpu::NumberFormat::Unorm;
    key.write_masks[0] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    PipelinePrecompileTest::MissingModules(cache, selection);
    for (const auto primitive :
         {AmdGpu::PrimitiveType::TriangleList, AmdGpu::PrimitiveType::TriangleStrip,
          AmdGpu::PrimitiveType::RectList, AmdGpu::PrimitiveType::QuadList}) {
        const bool generated = primitive == AmdGpu::PrimitiveType::RectList ||
                               primitive == AmdGpu::PrimitiveType::QuadList;
        if (generated && !instance.IsTessellationSupported())
            continue;
        key.prim_type = primitive;
        selection.modules[u32(Shader::SwStage::Vertex)] = generated ? *quad : *vertex;
        ud[0] = 123;
        auto prepared = PipelinePrecompileTest::Consume(
            cache, selection, primitive == AmdGpu::PrimitiveType::TriangleStrip);
        auto inline_pipeline = PipelinePrecompileTest::Inline(cache, selection);
        auto deferred_pipeline = PipelinePrecompileTest::Deferred(cache, selection);
        auto early_pipeline = PipelinePrecompileTest::Early(cache, selection);
        VideoCore::UniqueImage target{device, instance.GetAllocator()};
        target.Create({.imageType = vk::ImageType::e2D,
                       .format = vk::Format::eR8G8B8A8Unorm,
                       .extent = {16, 16, 1},
                       .mipLevels = 1,
                       .arrayLayers = 1,
                       .samples = vk::SampleCountFlagBits::e1,
                       .tiling = vk::ImageTiling::eOptimal,
                       .usage = vk::ImageUsageFlagBits::eColorAttachment |
                                vk::ImageUsageFlagBits::eTransferSrc});
        auto view = device
                        .createImageViewUnique(
                            {.image = vk::Image(target),
                             .viewType = vk::ImageViewType::e2D,
                             .format = vk::Format::eR8G8B8A8Unorm,
                             .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}},
                            nullptr, dispatch)
                        .value;
        VideoCore::Buffer readback{instance, 0, 16 * 16 * 4, VideoCore::MemoryType::HostCached,
                                   "Pipeline prep readback"};
        bool first = true;
        for (const auto *pipeline : {prepared.get(), inline_pipeline.get(), deferred_pipeline.get(), early_pipeline.get()}) {
            scheduler.Record([&, pipeline, first](vk::CommandBuffer cmd) {
                const vk::ImageMemoryBarrier2 begin{
                    .srcStageMask =
                        first ? vk::PipelineStageFlags2{} : vk::PipelineStageFlagBits2::eCopy,
                    .srcAccessMask =
                        first ? vk::AccessFlags2{} : vk::AccessFlagBits2::eTransferRead,
                    .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                    .oldLayout =
                        first ? vk::ImageLayout::eUndefined : vk::ImageLayout::eTransferSrcOptimal,
                    .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .image = vk::Image(target),
                    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
                cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &begin},
                                     dispatch);
                DynamicState state{};
                state.Invalidate();
                state.viewports.push_back({0, 0, 16, 16, 0, 1});
                state.scissors.push_back({{0, 0}, {16, 16}});
                state.line_width = 1;
                state.color_write_masks.fill(key.write_masks[0]);
                state.Commit(instance, cmd);
                cmd.setVertexInputEXT({}, {}, dispatch);
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle(), dispatch);
                const vk::RenderingAttachmentInfo attachment{
                    .imageView = *view,
                    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .loadOp = vk::AttachmentLoadOp::eClear,
                    .storeOp = vk::AttachmentStoreOp::eStore,
                    .clearValue = {.color = {.float32 = std::array<float, 4>{0.f, 0.f, 0.f, 0.f}}}};
                cmd.beginRendering({.renderArea = {{0, 0}, {16, 16}},
                                    .layerCount = 1,
                                    .colorAttachmentCount = 1,
                                    .pColorAttachments = &attachment},
                                   dispatch);
                cmd.draw(primitive == AmdGpu::PrimitiveType::QuadList ? 4 : 3, 1, 0, 0, dispatch);
                cmd.endRendering(dispatch);
                auto copy_barrier = begin;
                copy_barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
                copy_barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
                copy_barrier.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
                copy_barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
                copy_barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
                copy_barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
                cmd.pipelineBarrier2(
                    {.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &copy_barrier},
                    dispatch);
                const vk::BufferImageCopy copy{
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageExtent = {16, 16, 1}};
                cmd.copyImageToBuffer(vk::Image(target), vk::ImageLayout::eTransferSrcOptimal,
                                      readback.Handle(), copy, dispatch);
                const vk::MemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                                              .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                              .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                              .dstAccessMask = vk::AccessFlagBits2::eHostRead};
                cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &host}, dispatch);
            });
            scheduler.Finish();
            readback.Invalidate(0, 16 * 16 * 4);
            for (u32 i = 0; i < 16 * 16 * 4; i += 4) {
                const auto *pixel = readback.mapped_data.data() + i;
                if (pixel[0] != 0 || pixel[1] != 255 || pixel[2] != 0 || pixel[3] != 255)
                    std::fprintf(stderr, "%s pixel %u: %u %u %u %u\n",
                                 first ? "prepared" : "inline", i / 4, pixel[0], pixel[1], pixel[2],
                                 pixel[3]);
                assert(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255);
            }
            first = false;
        }
    }
    puts("PASS: triangle, required triangle-strip, generated rect/quad pipelines "
         "render identical pixels after snapshot release");

    // Real compute selection, preparation, key validation, adoption and dispatch.
    alignas(256) std::array<u32, 128> guest_code{};
    guest_code[0] = 0xBEEB03FF;
    guest_code[1] = 1;
    AmdGpu::BinaryInfo binary{};
    binary.signature = AmdGpu::BinaryInfo::signature_ref;
    binary.length = 8;
    auto &regs = gpu.GetCsRegs();
    regs.address = reinterpret_cast<uintptr_t>(guest_code.data()) >> 8;
    regs.num_thread_x.full = 64;
    regs.num_thread_y.full = regs.num_thread_z.full = 1;
    constexpr u32 Words = 128;
    VideoCore::Buffer output{instance, 0, Words * 4, VideoCore::MemoryType::HostCached,
                             "Prepared compute readback"};
    for (const bool prepared : {true, false}) {
        binary.shader_hash = prepared ? 0x12345 : 0x23456;
        std::memcpy(guest_code.data() + 4, &binary, sizeof(binary));
        PrepWorker missing{};
        assert(!cache.PrepareComputePipeline(regs, missing) && missing.missing_shader);
        const auto *info = PipelinePrecompileTest::InstallCompute(cache, regs, *compute, prepared);
        const auto generation = cache.ProgramGeneration();
        if (prepared) {
            auto unseen_permutation = regs;
            unseen_permutation.num_thread_x.full = 32;
            PrepWorker different{};
            assert(!cache.PrepareComputePipeline(unseen_permutation, different) && different.missing_shader);
            PipelinePrecompileTest::ScanCompute(cache);
            regs.user_data[0] = 999; // Owned queued snapshot must survive changed register data.
        }
        const bool deferred = !prepared && !(std::getenv("BB_FIRST_USE_COMPUTE") &&
                                             std::getenv("BB_FIRST_USE_COMPUTE")[0] == '0');
        std::function<void()> release;
        if (deferred) release = PipelinePrecompileTest::GateCompute(cache);
        auto selected = std::async(std::launch::async, [&] { return cache.GetComputePipeline(); });
        const bool frontend_ready = selected.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!frontend_ready && release) { release(); release = {}; }
        assert(frontend_ready);
        const auto *pipeline = selected.get();
        if (deferred) {
            assert(pipeline->NativePending() && pipeline->GetLayout());
            assert(cache.GetComputePipeline() == pipeline);
            auto native = std::async(std::launch::async, [pipeline] { return pipeline->Handle(); });
            assert(native.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
            release();
            assert(native.get() && !pipeline->NativePending());
            puts("PASS: production compute frontend returns while compiler is gated; layout is usable, bind waits and owned state survives constructor thread exit");
        }
        if (!prepared) assert(cache.ProgramGeneration() == generation + 1);
        assert(&pipeline->GetStage(Shader::SwStage::Compute) == info);
        PipelinePrecompileTest::CheckComputeAdoption(cache, prepared);
        std::memset(output.mapped_data.data(), 0, Words * 4);
        output.Flush(0, Words * 4);
        scheduler.Record([&, pipeline](vk::CommandBuffer cmd) {
            const vk::DescriptorBufferInfo buffer{output.Handle(), 0, Words * 4};
            const vk::WriteDescriptorSet write{.dstBinding = 0,
                                               .descriptorCount = 1,
                                               .descriptorType = vk::DescriptorType::eStorageBuffer,
                                               .pBufferInfo = &buffer};
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle(), dispatch);
            cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, pipeline->GetLayout(), 0,
                                     write, dispatch);
            cmd.dispatch(2, 1, 1, dispatch);
            const vk::MemoryBarrier2 barrier{.srcStageMask =
                                                 vk::PipelineStageFlagBits2::eComputeShader,
                                             .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
                                             .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                             .dstAccessMask = vk::AccessFlagBits2::eHostRead};
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier}, dispatch);
        });
        scheduler.Finish();
        output.Invalidate(0, Words * 4);
        for (u32 i = 0; i < Words; ++i) {
            u32 actual;
            std::memcpy(&actual, output.mapped_data.data() + i * 4, 4);
            assert(actual == (i ^ 0xA5A5u));
        }
    }
    PipelinePrecompileTest::ComputeDrain(cache);
    puts("PASS: prepared and required production compute pipelines validate "
         "keys, rebind runtime stages and produce exact GPU readback; caches "
         "merged after drain");
}
