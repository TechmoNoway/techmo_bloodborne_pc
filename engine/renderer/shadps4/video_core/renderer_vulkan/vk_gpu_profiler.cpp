// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>

#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

void GpuProfiler::Init(const Instance& instance, Scheduler& scheduler) {
    const char* env = std::getenv("BB_GPU_PROFILE");
    if (!env || env[0] != '1' || instance_ptr) {
        return;
    }
    instance_ptr = new GpuProfiler(instance, scheduler);
}

void GpuProfiler::Shutdown() {
    if (!instance_ptr) return;
    instance_ptr->scheduler.Finish();
    delete instance_ptr;
    instance_ptr = nullptr;
}

GpuProfiler::~GpuProfiler() {
    if (csv) std::fclose(csv);
}

GpuProfiler::GpuProfiler(const Instance& instance, Scheduler& scheduler_)
    : device{instance.GetDevice()}, scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo info = {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlices * SliceQueries,
    };
    pool = Check<"create timestamp query pool">(device.createQueryPoolUnique(info));
    device.resetQueryPool(*pool, 0, NumSlices * SliceQueries);
    period_ns = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    for (auto& k : keys) {
        k.reserve(SliceQueries);
    }
    std::printf("GPU profile: on (timestamp period %.2f ns)\n", period_ns);
    if (const char* path = std::getenv("BB_GPU_LOG"); path && *path) {
        csv = std::fopen(path, "w");
        if (csv) {
            std::fprintf(csv, "sample,gpu_interval_ms,timestamp_count,skipped_frames,labelled_work_ms,between_submissions_ms\n");
            std::fflush(csv);
        }
    }
}

void GpuProfiler::WriteTimestamp(u64 key) {
    if (!recording) return;
    if (used[slice] + 1 >= SliceQueries) {
        return; // the frame's slice is full: the rest of the frame goes to the last label
    }
    // Outside render passes: radv_CmdWriteTimestamp2 crashed inside some. Marks sit where a
    // pass, dispatch or submission ends anyway.
    scheduler.EndRendering();
    const u32 query = slice * SliceQueries + used[slice]++;
    keys[slice].push_back(key);
    current = key;
    scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
    });
}

void GpuProfiler::BeginFrame() {
    // Close the frame: one more timestamp without a label.
    if (recording && used[slice] > 0 && used[slice] < SliceQueries) {
        scheduler.EndRendering();
        const u32 query = slice * SliceQueries + used[slice]++;
        scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
            cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
        });
        pending[slice] = true;
        completion_ticks[slice] = scheduler.CurrentTick();
    }
    slice = (slice + 1) % NumSlices;
    // The oldest slice: its frame was submitted four frames ago.
    if (pending[slice] && !Collect(slice)) {
        // The GPU still owns these queries. Never reset or append to this slice, and
        // never delay the renderer just to collect diagnostic measurements.
        recording = false;
        ++skipped_frames;
        Print();
        return;
    }
    if (used[slice]) {
        device.resetQueryPool(*pool, slice * SliceQueries, used[slice]);
    }
    used[slice] = 0;
    keys[slice].clear();
    pending[slice] = false;
    recording = true;
    Print();
}

bool GpuProfiler::Collect(u32 which) {
    // Completion also covers recording threads and the entire submission that owns
    // the closing timestamp. Host query reset is only legal after that work finishes.
    if (!scheduler.IsFree(completion_ticks[which])) return false;
    const u32 count = used[which];
    std::vector<u64> stamps(count);
    // Even if a driver does not expose results yet, leave them pending and try later.
    const auto result = device.getQueryPoolResults(
        *pool, which * SliceQueries, count, count * sizeof(u64), stamps.data(), sizeof(u64),
        vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    double labelled_work_ms = 0, between_submissions_ms = 0;
    for (u32 i = 0; i + 1 < count; ++i) {
        const double ms = double(stamps[i + 1] - stamps[i]) * period_ns / 1e6;
        // Scheduler's 0x5B317 label denotes the gap between submissions, including
        // pacing/CPU delays. It must not be interpreted as render-pass execution time.
        if (keys[which][i] == 0x5B317ull) between_submissions_ms += ms;
        else labelled_work_ms += ms;
        auto& total = totals[keys[which][i]];
        total.ms += ms;
        ++total.segments;
    }
    ++collected_frames;
    if (csv && count >= 2) {
        std::fprintf(csv, "%llu,%.6f,%u,%llu,%.6f,%.6f\n", static_cast<unsigned long long>(collected_frames),
                     double(stamps.back() - stamps.front()) * period_ns / 1e6, count,
                     static_cast<unsigned long long>(skipped_frames), labelled_work_ms,
                     between_submissions_ms);
        if (++csv_unflushed >= 60) {
            std::fflush(csv);
            csv_unflushed = 0;
        }
    }
    ++frames;
    return true;
}

void GpuProfiler::Print() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5) || frames == 0) {
        return;
    }
    window = now;
    std::vector<std::pair<u64, Total>> sorted(totals.begin(), totals.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double sum = 0;
    for (const auto& [key, total] : sorted) {
        sum += total.ms;
    }
    std::printf("GPU profile: %.2f ms/frame over %llu frames, %zu labels; %llu skipped (busy query slices)\n", sum / frames,
                static_cast<unsigned long long>(frames), sorted.size(),
                static_cast<unsigned long long>(skipped_frames));
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 30); ++i) {
        const auto& [key, total] = sorted[i];
        std::printf("  %6.3f ms/frame %5.1f/frame  %s\n", total.ms / frames,
                    double(total.segments) / frames, described[key].c_str());
    }
    totals.clear();
    frames = 0;
}

} // namespace Vulkan
