// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: speculative draw preparation on worker threads (see vk_draw_prep.h).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bbport_threads.h"
#include "bbport_toggles.h"
#include "common/thread.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/graphics_scan_policy.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include <xxhash.h>

namespace Vulkan {

namespace {

bool IsDirectDraw(AmdGpu::PM4ItOpcode opcode) {
    using AmdGpu::PM4ItOpcode;
    return opcode == PM4ItOpcode::DrawIndex2 || opcode == PM4ItOpcode::DrawIndexOffset2 ||
           opcode == PM4ItOpcode::DrawIndexAuto;
}

/// Walks the type-3 packets of a command buffer (type-2 padding skipped).
template <typename Func>
void ForEachPacket(std::span<const u32> commands, Func&& func) {
    for (size_t at = 0; at < commands.size();) {
        const auto* header = reinterpret_cast<const AmdGpu::PM4Header*>(commands.data() + at);
        if (header->type == 2) {
            ++at;
            continue;
        }
        if (header->type != 3) {
            return;
        }
        const size_t words = header->type3.NumWords() + 1;
        if (at + words > commands.size()) {
            return;
        }
        func(header);
        at += words;
    }
}

u32 DefaultWorkerCount() {
    if (const char* env = std::getenv("BB_PREP_WORKERS")) {
        return static_cast<u32>(std::clamp(std::atoi(env), 0, 16));
    }
    // Workers are SCHED_IDLE and claim whole buffers, so more of them only use more idle
    // cores: half the hardware threads (Steam Deck 4, 16-thread desktop 8).
    return std::clamp<u32>(BbThreads::Available() / 2, 1, 8);
}

/// Flattened user data of one submission: chunks never move, so the GPU thread can read a
/// prepared draw's data while the worker keeps appending.
struct FlatArena {
    static constexpr size_t ChunkWords = 16 * 1024;
    std::vector<std::unique_ptr<u32[]>> chunks;
    size_t used = ChunkWords;

    const u32* Append(const std::vector<u32>& data) {
        return AppendArray(data.data(), data.size());
    }

    /// Trivially copyable elements, 8-byte aligned (sharps hold u64 bit fields).
    template <typename T>
    const T* AppendArray(const T* data, size_t count) {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= 8);
        const size_t words = (count * sizeof(T) + sizeof(u32) - 1) / sizeof(u32);
        used = (used + 1) & ~size_t(1);
        if (chunks.empty() || words > ChunkWords - std::min(used, ChunkWords)) {
            chunks.push_back(std::make_unique<u32[]>(std::max(ChunkWords, words)));
            used = 0;
        }
        u32* dst = chunks.back().get() + used;
        if (count) {
            std::memcpy(dst, data, count * sizeof(T));
        }
        used += words;
        return reinterpret_cast<const T*>(dst);
    }
};

/// Reads the stage's resource sharps from the worker's flattened user data, so the GPU thread
/// only binds them (the sharp fetches and texture-description hashing were ~7% of it).
void PrepareResources(const Shader::Info& info, PreparedStage& stage, FlatArena& arena) {
    thread_local std::vector<AmdGpu::Image> images;
    thread_local std::vector<u64> hashes;
    thread_local std::vector<AmdGpu::Sampler> samplers;
    thread_local std::vector<AmdGpu::Buffer> buffers;
    images.clear();
    hashes.clear();
    samplers.clear();
    buffers.clear();
    for (const auto& res : info.images) {
        images.push_back(res.GetSharp(info));
        hashes.push_back(ImageDescHash(images.back(), res));
    }
    for (const auto& res : info.samplers) {
        samplers.push_back(res.GetSharp(info));
    }
    for (const auto& res : info.buffers) {
        buffers.push_back(res.IsSpecial() ? AmdGpu::Buffer::Null() : res.GetSharp(info));
    }
    stage.image_sharps = arena.AppendArray(images.data(), images.size());
    stage.image_hashes = arena.AppendArray(hashes.data(), hashes.size());
    stage.sampler_sharps = arena.AppendArray(samplers.data(), samplers.size());
    stage.buffer_sharps = arena.AppendArray(buffers.data(), buffers.size());
    stage.num_images = static_cast<u32>(images.size());
    stage.num_samplers = static_cast<u32>(samplers.size());
    stage.num_buffers = static_cast<u32>(buffers.size());
}

/// Rasterizer::BindVertexBuffers' pure part (GraphicsPipeline::GetVertexInputs for the dynamic
/// vertex input path, and the merge of the stream memory into ranges).
void PrepareVertexInputs(const Shader::Info& vs, const Shader::Gcn::FetchShaderData& fetch,
                         const AmdGpu::Regs& regs, PreparedVertexInputs& out, FlatArena& arena) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    thread_local std::vector<vk::VertexInputAttributeDescription2EXT> attributes;
    thread_local std::vector<vk::VertexInputBindingDescription2EXT> bindings;
    thread_local std::vector<AmdGpu::Buffer> buffers;
    thread_local std::vector<PreparedVertexInputs::Range> ranges, merged;
    thread_local std::vector<u8> range_index;
    attributes.clear();
    bindings.clear();
    buffers.clear();
    ranges.clear();
    merged.clear();
    range_index.clear();
    for (const auto& attrib : fetch.attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs);
        // The V# may come from user data the guest is still writing: an unknown format leaves
        // the inputs to the GPU thread, which reads them once the draw is submitted (the
        // assertion in SurfaceFormat stopped the process here).
        const vk::Format format =
            LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        if (format == vk::Format::eUndefined &&
            buffer.GetDataFmt() != AmdGpu::DataFormat::FormatInvalid) {
            out.valid = false;
            return;
        }
        attributes.push_back({
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = format,
            .offset = 0,
        });
        bindings.push_back({
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
            .divisor = step_rate == InstanceIdType::OverStepRate0 ? regs.vgt_instance_step_rate_0
                       : step_rate == InstanceIdType::OverStepRate1
                           ? regs.vgt_instance_step_rate_1
                           : 1u,
        });
        buffers.push_back(buffer);
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            ranges.push_back({buffer.base_address, buffer.base_address + buffer.GetSize()});
        }
    }
    std::ranges::sort(ranges, {}, &PreparedVertexInputs::Range::base);
    for (const auto& range : ranges) {
        if (merged.empty() || merged.back().end < range.base) {
            merged.push_back(range);
        } else {
            merged.back().end = std::max(merged.back().end, range.end);
        }
    }
    for (const auto& buffer : buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto it = std::ranges::find_if(merged, [&](const auto& range) {
                return buffer.base_address >= range.base && buffer.base_address < range.end;
            });
            range_index.push_back(static_cast<u8>(it - merged.begin()));
        }
    }
    out.attributes = arena.AppendArray(attributes.data(), attributes.size());
    out.bindings = arena.AppendArray(bindings.data(), bindings.size());
    out.buffers = arena.AppendArray(buffers.data(), buffers.size());
    out.range_index = arena.AppendArray(range_index.data(), range_index.size());
    out.ranges = arena.AppendArray(merged.data(), merged.size());
    out.count = static_cast<u32>(attributes.size());
    out.num_ranges = static_cast<u32>(merged.size());
    out.buffers_hash =
        buffers.empty() ? 0 : XXH3_64bits(buffers.data(), buffers.size() * sizeof(AmdGpu::Buffer));
    out.valid = true;
}

} // namespace

void DrawPreparation::ApplyPreparationRegisters(AmdGpu::Regs& regs, const AmdGpu::PM4Header* header,
                                               u64& checksum, AmdGpu::RegDirty* dirty) {
    using AmdGpu::PM4ItOpcode;
    const auto mark_compute = [&] { if (dirty) dirty->MarkField(regs, regs.cs_program); };
    if (header->type3.opcode == PM4ItOpcode::ClearState) {
        const auto saved_compute = regs.cs_program;
        AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(regs, header, checksum, dirty);
        regs.cs_program = saved_compute;
        mark_compute();
        return;
    }
    AmdGpu::Liverpool::ApplyGraphicsRegisterPacket(regs, header, checksum, dirty);
    switch (header->type3.opcode) {
    case PM4ItOpcode::SetShReg: {
        const auto* packet = reinterpret_cast<const AmdGpu::PM4CmdSetData*>(header);
        const auto words = header->type3.NumWords() - 1;
        const auto offset = packet->reg_offset;
        constexpr u32 CsBegin = 0x200, CsWords = sizeof(AmdGpu::ComputeProgram) / sizeof(u32);
        if (offset >= CsBegin && offset - CsBegin < CsWords && words <= CsWords - (offset - CsBegin)) {
            std::memcpy(reinterpret_cast<u32*>(&regs.cs_program) + offset - CsBegin, header + 2,
                        words * sizeof(u32));
            mark_compute();
        }
        break;
    }
    case PM4ItOpcode::DispatchDirect: {
        const auto* packet = reinterpret_cast<const AmdGpu::PM4CmdDispatchDirect*>(header);
        regs.cs_program.dim_x = packet->dim_x;
        regs.cs_program.dim_y = packet->dim_y;
        regs.cs_program.dim_z = packet->dim_z;
        regs.cs_program.dispatch_initiator = packet->dispatch_initiator;
        mark_compute();
        break;
    }
    default: break;
    }
}

DrawPreparation::DrawPreparation(PipelineCache& pipeline_cache_)
    : pipeline_cache{pipeline_cache_} {
    worker_count = DefaultWorkerCount();
    const char* retry = std::getenv("BB_PIPELINE_RETRY");
    retry_enabled = worker_count && (!retry || retry[0] != '0');
    if (worker_count) {
        scanner = std::jthread([this](std::stop_token stop) { ScannerLoop(stop); });
    }
    for (u32 i = 0; i < worker_count; ++i) {
        workers.emplace_back([this, i](std::stop_token stop) { WorkerLoop(stop, i); });
    }
    if (retry_enabled) pipeline_cache.SetPreparationWake([this] {
        std::scoped_lock lk{mutex}; // Pair the publication with wait's predicate.
        cv.notify_all();
    });
    std::printf("GPU: draw preparation workers: %u (+1 scanner, %u hardware threads)\n",
                worker_count, BbThreads::Available());
}

DrawPreparation::~DrawPreparation() {
    pipeline_cache.SetPreparationWake({}); // No callback can outlive this condition variable.
    for (auto& worker : workers) {
        worker.request_stop();
    }
    if (scanner.joinable()) {
        scanner.request_stop();
    }
    cv.notify_all();
    workers.clear();
    scanner = {};
    std::printf("Pipeline retries: %llu passes, %llu draws recovered\n",
                static_cast<unsigned long long>(retry_passes.load()),
                static_cast<unsigned long long>(retry_draws.load()));
    std::printf("Compute lookahead: %llu scanner attempts, %llu known selections\n",
                static_cast<unsigned long long>(compute_scan_attempts.load()),
                static_cast<unsigned long long>(compute_scan_ready.load()));
}

std::shared_ptr<DrawPreparation::Submission> DrawPreparation::Build(
    std::span<const u32> commands) {
    if (!Enabled()) {
        return {};
    }
    auto submission = std::make_shared<Submission>();
    submission->commands.assign(commands.begin(), commands.end());
    ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
        submission->num_draws += IsDirectDraw(header->type3.opcode);
    });
    submission->draws = std::make_unique<PreparedDraw[]>(submission->num_draws);
    return submission;
}

void DrawPreparation::Enqueue(u64 seq, std::shared_ptr<Submission> submission) {
    if (!submission) {
        return;
    }
    submission->seq = seq;
    {
        std::scoped_lock lk{mutex};
        submissions.push_back(std::move(submission));
    }
    cv.notify_all();
}

void DrawPreparation::BeginSubmission(u64 seq, const AmdGpu::Regs& regs, u64 reg_checksum) {
    current_draw = 0;
    current.reset();
    if (!Enabled()) {
        return;
    }
    gpu_seq.store(seq, std::memory_order_release);
    std::scoped_lock lk{mutex};
    if (!baseline_ready) {
        initial_regs = std::make_unique<AmdGpu::Regs>(regs);
        tail_regs = std::make_unique<AmdGpu::Regs>(regs);
        initial_regs->cs_program = tail_regs->cs_program = pipeline_cache.GetComputeRegisters();
        initial_checksum = tail_checksum = reg_checksum;
        baseline_seq = tail_seq = seq;
        baseline_ready = true;
        cv.notify_all();
    }
    // Idle helpers can starve on a busy CPU. Rather than queueing without bound, restart
    // them from the GPU thread's own state (one register copy) once the scanner lags.
    constexpr u64 MaxScanLag = 64;
    if (!submissions.empty() && seq > tail_seq + MaxScanLag &&
        submissions.front()->seq < seq && !submissions.front()->scanned.load()) {
        *tail_regs = regs;
        tail_regs->cs_program = pipeline_cache.GetComputeRegisters();
        tail_seq = seq;
        tail_checksum = reg_checksum;
        while (!submissions.empty() && submissions.front()->seq < seq) {
            submissions.pop_front();
        }
        ++rebases;
        cv.notify_all();
    }
    if (!submissions.empty() && seq >= submissions.front()->seq &&
        seq - submissions.front()->seq < submissions.size()) {
        current = submissions[seq - submissions.front()->seq];
    }
}

void DrawPreparation::EndSubmission() {
    if (current) {
        current->gpu_done.store(true, std::memory_order_release);
        current.reset();
    }
    if (Enabled()) {
        Collect();
    }
}

const PreparedDraw* DrawPreparation::NextDraw() {
    if (!current || current_draw >= current->num_draws) {
        return nullptr;
    }
    const PreparedDraw* draw = &current->draws[current_draw++];
    if (BbToggle::Disabled(BbToggle::DrawPreparation)) {
        return nullptr;
    }
    return draw;
}

void DrawPreparation::Collect() {
    std::scoped_lock lk{mutex};
    while (!submissions.empty()) {
        const auto& front = submissions.front();
        if (!baseline_ready || !front->gpu_done.load(std::memory_order_acquire)) {
            break;
        }
        if (front->seq >= baseline_seq) {
            // Its delta moves the restart state to the start of the next buffer.
            if (!front->scanned.load(std::memory_order_acquire)) {
                break;
            }
            front->delta.Apply(*tail_regs);
            tail_seq = front->seq + 1;
            tail_checksum = front->end_checksum;
        }
        submissions.pop_front();
    }
}

void DrawPreparation::ScannerLoop(std::stop_token stop) {
    Common::SetCurrentThreadName("bb:DrawScan");
    BbThreads::MakeBackground();
    std::unique_ptr<AmdGpu::Regs> regs;
    u64 checksum = 0;
    u64 next_seq = 0;
    {
        std::unique_lock lk{mutex};
        cv.wait(lk, stop, [&] { return baseline_ready; });
        if (stop.stop_requested()) {
            return;
        }
        regs = std::make_unique<AmdGpu::Regs>(*initial_regs);
        checksum = initial_checksum;
        next_seq = baseline_seq;
    }
    AmdGpu::RegDirty dirty;
    PrepWorker compute_worker, graphics_worker;
    PipelineSelection graphics_sel{};
    graphics_sel.regs = regs.get();
    graphics_sel.worker = &graphics_worker;
    const char* gfx_option = std::getenv("BB_GRAPHICS_SCAN");
    const bool graphics_scan = !gfx_option || gfx_option[0] != '0';
    const char* spread_option = std::getenv("BB_GRAPHICS_SCAN_SPREAD");
    const bool spread = !spread_option || spread_option[0] != '0';
    std::printf("Graphics scanner sampling: %s, maximum %u selections per buffer\n", spread ? "spread" : "head", GraphicsScan::Budget);
    const char* option = std::getenv("BB_COMPUTE_SCAN");
    const bool compute_scan = !option || option[0] != '0';
    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> submission;
        {
            std::unique_lock lk{mutex};
            cv.wait(lk, stop, [&] {
                return next_seq < tail_seq ||
                       (!submissions.empty() && submissions.back()->seq >= next_seq);
            });
            if (stop.stop_requested()) {
                return;
            }
            if (next_seq < tail_seq) {
                // Rebased by the GPU thread: continue from its state.
                *regs = *tail_regs;
                checksum = tail_checksum;
                next_seq = tail_seq;
                continue;
            }
            // Unscanned buffers are only collected by a rebase, so `next_seq` is still queued.
            submission = submissions[next_seq - submissions.front()->seq];
        }
        submission->start_checksum = checksum;
        dirty.Clear();
        unsigned compute_attempts = 0, graphics_attempts = 0, graphics_ordinal = 0;
        ForEachPacket(submission->commands, [&](const AmdGpu::PM4Header* header) {
            ApplyPreparationRegisters(*regs, header, checksum, &dirty);
            const bool direct = IsDirectDraw(header->type3.opcode);
            const bool sample = direct && (!spread || GraphicsScan::Sample(graphics_ordinal, submission->num_draws));
            if (direct) ++graphics_ordinal;
            if (graphics_scan && sample && graphics_attempts < GraphicsScan::Budget &&
                regs->stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs &&
                submission->seq >= gpu_seq.load(std::memory_order_acquire) &&
                !submission->gpu_done.load(std::memory_order_acquire)) {
                ++graphics_attempts;
                ++graphics_scan_attempts;
                graphics_worker.stages.clear();
                graphics_worker.failed = graphics_worker.missing_shader = false;
                graphics_sel.draw_indirect_params = {};
                if (pipeline_cache.PrepareGraphicsPipeline(graphics_sel)) ++graphics_scan_ready;
            }

            // Queue known compute selections before publishing this buffer to the
            // draw workers. Cold modules remain on the existing required path;
            // workers retry them after publication. Keep scanner work bounded.
            if (compute_scan && compute_attempts < 8 &&
                header->type3.opcode == AmdGpu::PM4ItOpcode::DispatchDirect &&
                submission->seq >= gpu_seq.load(std::memory_order_acquire) &&
                !submission->gpu_done.load(std::memory_order_acquire) &&
                (regs->cs_program.dispatch_initiator & 1) && regs->cs_program.dim_x &&
                regs->cs_program.dim_y && regs->cs_program.dim_z) {
                ++compute_attempts;
                ++compute_scan_attempts;
                compute_worker.stages.clear();
                compute_worker.failed = compute_worker.missing_shader = false;
                if (pipeline_cache.PrepareComputePipeline(regs->cs_program, compute_worker))
                    ++compute_scan_ready;
            }
        });
        submission->delta.Capture(*regs, dirty);
        submission->end_checksum = checksum;
        {
            std::scoped_lock lk{mutex};
            submission->scanned.store(true, std::memory_order_release);
        }
        ++next_seq;
        cv.notify_all();
    }
}

void DrawPreparation::WorkerLoop(std::stop_token stop, u32 index) {
    Common::SetCurrentThreadName(("bb:DrawPrep" + std::to_string(index)).c_str());
    BbThreads::MakeBackground();
    auto regs = std::make_unique<AmdGpu::Regs>();
    constexpr u64 NoPosition = ~0ull;
    u64 position = NoPosition; ///< `regs` holds the state at the start of this buffer
    PipelineSelection sel{};
    sel.regs = regs.get();
    PrepWorker prep_worker{};
    sel.worker = &prep_worker;
    FlatArena arena;
    std::vector<std::shared_ptr<Submission>> path;

    // The nearest scanned buffer the GPU thread has not reached and no worker has claimed.
    const auto claimable = [&]() -> std::shared_ptr<Submission> {
        const u64 gpu = gpu_seq.load(std::memory_order_acquire);
        const auto generation = pipeline_cache.ProgramGeneration();
        for (const auto& submission : submissions) {
            if (submission->seq >= gpu && submission->seq >= tail_seq &&
                submission->scanned.load(std::memory_order_acquire) &&
                submission->retry.Claimable(generation, retry_enabled) &&
                !submission->gpu_done.load(std::memory_order_acquire)) {
                return submission;
            }
        }
        return {};
    };

    while (!stop.stop_requested()) {
        std::shared_ptr<Submission> target;
        bool retry_pass = false;
        {
            std::unique_lock lk{mutex};
            // gpu_seq moves without the lock: keep the candidate the predicate found.
            cv.wait(lk, stop, [&] {
                target = baseline_ready ? claimable() : nullptr;
                return target != nullptr;
            });
            if (stop.stop_requested() || !target) {
                return;
            }
            retry_pass = !target->retry.FirstPass();
            target->retry.Claim(pipeline_cache.ProgramGeneration());
            // Buffers between this worker's state and the target: their deltas. A position
            // already collected (or past the target) restarts from the queue's tail state.
            if (position == NoPosition || position < tail_seq || position > target->seq) {
                *regs = *tail_regs;
                position = tail_seq;
            }
            const u64 front = submissions.front()->seq;
            path.clear();
            for (u64 seq = position; seq < target->seq; ++seq) {
                path.push_back(submissions[seq - front]);
            }
        }
        for (const auto& step : path) {
            step->delta.Apply(*regs);
        }
        path.clear();
        u64 checksum = target->start_checksum;
        u32 ordinal = 0;
        bool missing_shader = false;
        if (retry_pass) ++retry_passes;
        arena = FlatArena{};
        ForEachPacket(target->commands, [&](const AmdGpu::PM4Header* header) {
            ApplyPreparationRegisters(*regs, header, checksum);
            if (header->type3.opcode == AmdGpu::PM4ItOpcode::DispatchDirect &&
                target->seq >= gpu_seq.load(std::memory_order_acquire) &&
                (regs->cs_program.dispatch_initiator & 1) && regs->cs_program.dim_x &&
                regs->cs_program.dim_y && regs->cs_program.dim_z) {
                prep_worker.stages.clear();
                prep_worker.failed = prep_worker.missing_shader = false;
                pipeline_cache.PrepareComputePipeline(regs->cs_program, prep_worker);
                missing_shader |= prep_worker.missing_shader;
            }
            if (!IsDirectDraw(header->type3.opcode)) {
                return;
            }
            // Past the GPU thread the draws are no longer worth preparing.
            if (ordinal < target->num_draws &&
                target->seq >= gpu_seq.load(std::memory_order_acquire)) {
                auto& draw = target->draws[ordinal];
                if (draw.state.load(std::memory_order_acquire) == PreparedDraw::Ready) {
                    ++ordinal;
                    return; // Published resource pointers and arenas are immutable.
                }
                prep_worker.stages.clear();
                prep_worker.failed = false;
                prep_worker.missing_shader = false;
                sel.draw_indirect_params = {};
                if (pipeline_cache.PrepareGraphicsPipeline(sel)) {
                    draw.reg_checksum = checksum;
                    draw.key = sel.graphics_key;
                    draw.num_stages = static_cast<u32>(prep_worker.stages.size());
                    for (u32 i = 0; i < draw.num_stages; ++i) {
                        const auto& stage = prep_worker.stages[i];
                        draw.stages[i] = {
                            .program = stage.program,
                            .hash = stage.hash,
                            .hw_stage = stage.hw_stage,
                            .pgm_base = stage.pgm_base,
                            .flat = arena.Append(*stage.flat),
                            .flat_size = static_cast<u32>(stage.flat->size()),
                        };
                        PrepareResources(prep_worker.infos.at(stage.program), draw.stages[i],
                                         arena);
                    }
                    draw.vertex.valid = false;
                    const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
                    if (vs && sel.fetch_shader) {
                        PrepareVertexInputs(*vs, *sel.fetch_shader, *regs, draw.vertex, arena);
                    }
                    draw.state.store(PreparedDraw::Ready, std::memory_order_release);
                    if (retry_pass) ++retry_draws;
                } else {
                    missing_shader |= prep_worker.missing_shader;
                    draw.state.store(PreparedDraw::Unavailable, std::memory_order_release);
                }
            }
            ++ordinal;
        });
        // The arena's chunks live as long as the submission.
        // Ready draws may already be used by a recording thread. Never replace
        // arenas from an earlier pass; append ownership of newly prepared data.
        for (auto& chunk : arena.chunks) target->flat_chunks.push_back(std::move(chunk));
        {
            std::scoped_lock lk{mutex};
            target->retry.Complete(missing_shader);
        }
        cv.notify_all();
        position = target->seq + 1;
    }
}

void DrawPreparation::Count(bool was_used) {
    ++(was_used ? used : unused);
    static const bool stats = std::getenv("BB_FRAME_STATS") != nullptr;
    // The clock is read every 1024 draws: per draw it was 8% of the GPU thread.
    if (!stats || ((used + unused) & 1023) != 0) {
        return;
    }
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5)) {
        return;
    }
    window = now;
    std::printf("Draw preparation: %llu prepared pipelines used, %llu not (%u workers, "
                "%llu scanner rebases)\n",
                static_cast<unsigned long long>(used), static_cast<unsigned long long>(unused),
                worker_count, static_cast<unsigned long long>(rebases));
    std::printf("Graphics lookahead: %llu scanner attempts, %llu known selections\n",
                static_cast<unsigned long long>(graphics_scan_attempts.load()),
                static_cast<unsigned long long>(graphics_scan_ready.load()));
    std::printf("Compute lookahead: %llu scanner attempts, %llu known selections\n",
                static_cast<unsigned long long>(compute_scan_attempts.load()),
                static_cast<unsigned long long>(compute_scan_ready.load()));
    used = unused = 0;
}

} // namespace Vulkan
