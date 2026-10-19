// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <cstdio>
namespace BbHashReuse {
enum Reason : unsigned {
    Disabled, TrackingOff, NotGpuDirty, NotGpuSource, BackingChanged, MipsMissing,
    CpuDirty, NotFullyTracked, TrackingGap, MappingBusy, MappingChanged,
    HostWriteBusy, HostReferenceInvalid, HostWriteChanged, Count
};
inline constexpr std::array<const char*, Count> Names{
    "disabled", "tracking_off", "not_gpu_dirty", "not_gpu_source", "backing_changed", "mips_missing",
    "cpu_dirty", "not_fully_tracked", "tracking_gap", "mapping_busy", "mapping_changed",
    "host_write_busy", "host_reference_invalid", "host_write_changed"};
struct Conditions {
    bool enabled, tracking, gpu_dirty, gpu_source, same_backing, complete_mips,
         cpu_clean, fully_tracked, tracked_reference, mapping_idle, mapping_same,
         host_idle, host_reference, host_same;
};
constexpr std::uint32_t Blockers(const Conditions& c) {
    std::uint32_t mask = 0;
    const auto block = [&](Reason reason, bool failed) { if (failed) mask |= 1u << reason; };
    block(Disabled, !c.enabled); block(TrackingOff, !c.tracking);
    block(NotGpuDirty, !c.gpu_dirty); block(NotGpuSource, !c.gpu_source);
    block(BackingChanged, !c.same_backing); block(MipsMissing, !c.complete_mips);
    block(CpuDirty, !c.cpu_clean); block(NotFullyTracked, !c.fully_tracked);
    block(TrackingGap, !c.tracked_reference); block(MappingBusy, !c.mapping_idle);
    block(MappingChanged, c.tracked_reference && c.mapping_idle && !c.mapping_same);
    block(HostWriteBusy, !c.host_idle); block(HostReferenceInvalid, !c.host_reference);
    block(HostWriteChanged, c.host_idle && c.host_reference && !c.host_same);
    return mask;
}
inline std::atomic<std::uint64_t> Candidates{0}, Reused{0};
inline std::array<std::atomic<std::uint64_t>, Count> Blocked{};
inline void Note(std::uint32_t mask) {
    Candidates.fetch_add(1, std::memory_order_relaxed);
    if (!mask) Reused.fetch_add(1, std::memory_order_relaxed);
    while (mask) {
        const auto reason = std::countr_zero(mask); mask &= mask - 1;
        Blocked[reason].fetch_add(1, std::memory_order_relaxed);
    }
}
inline void Print() {
    // One stdio write keeps fields together when compiler/recording threads log.
    char line[1024];
    const auto hits = Reused.load(std::memory_order_relaxed);
    const auto candidates = Candidates.load(std::memory_order_relaxed);
    int count = std::snprintf(line, sizeof(line), "GPU hash reuse: candidates=%llu reused=%llu",
        static_cast<unsigned long long>(candidates), static_cast<unsigned long long>(hits));
    if (count < 0) return;
    std::size_t used = static_cast<std::size_t>(count);
    for (unsigned i=0;i<Count && used<sizeof(line);++i) {
        count = std::snprintf(line+used, sizeof(line)-used, " %s=%llu", Names[i],
            static_cast<unsigned long long>(Blocked[i].load(std::memory_order_relaxed)));
        if (count<0 || static_cast<std::size_t>(count)>=sizeof(line)-used) return;
        used += static_cast<std::size_t>(count);
    }
    std::printf("%s\n", line);
}

} // namespace BbHashReuse
