// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/pipeline_replay_metrics.h"
#include "common/hash.h"
#include "video_core/renderer_vulkan/pipeline_warmup_evidence.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <thread>
using namespace Vulkan::ReplayMetrics;
static uint64_t tick{}, reads{};
static uint64_t FakeClock() { ++reads; return tick; }
int main() {
    Vulkan::PipelineWarmupEvidence evidence;
    assert(evidence.Lookup("c_0x1").state == "not_run");
    evidence.Reset("profile_mismatch");
    assert(evidence.Lookup("c_0x1").state == "profile_mismatch");
    evidence.Reset("ready");
    assert(evidence.Lookup("c_0x1").state == "absent");
    evidence.Record("c_0x1", false, "missing_compiler_stamp");
    assert(evidence.Lookup("c_0x1").state == "rejected");
    assert(evidence.Lookup("c_0x1").rejection == "missing_compiler_stamp");
    evidence.Record("c_0x1", true, "none");
    evidence.Record("c_0x1", false, "invalid_record");
    assert(evidence.Lookup("c_0x1").state == "loaded");
    assert(evidence.Lookup("g_0x1").state == "absent");
    evidence.Reset("disabled");
    assert(evidence.Lookup("c_0x1").state == "disabled");
    assert(HashCombine(u64{0x123456}, u64{2}) == 0x1c16fc956ULL);
    assert(HashCombine(u64{0xffffffffffffffffULL}, u64{4095}) == 0xefffffff61c88648ULL);
    Capture capture{FakeClock};
    capture.Start();
    tick = 5;
    {
        Scope state{Phase::PipelineState};
        tick = 15;
        { Scope native{Phase::NativePipeline}; tick = 70; }
        tick = 80;
        state.Stop();
        const auto at_stop = reads;
        state.Stop();
        assert(reads == at_stop);
    }
    tick = 100;
    capture.Finish();
    assert(capture.Nanoseconds()[size_t(Phase::Other)] == 25);
    assert(capture.Nanoseconds()[size_t(Phase::PipelineState)] == 20);
    assert(capture.Nanoseconds()[size_t(Phase::NativePipeline)] == 55);
    uint64_t sum{};
    for (auto ns : capture.Nanoseconds()) sum += ns;
    assert(sum == 100 && current == nullptr);
    const auto idle_reads = reads;
    { Scope disabled{Phase::NativePipeline}; }
    capture.Finish();
    assert(reads == idle_reads);
    Capture exception{FakeClock};
    exception.Start();
    try {
        Scope reading{Phase::MetadataRead};
        tick = 120;
        throw std::runtime_error("fixture");
    } catch (const std::exception&) {}
    tick = 130;
    std::thread worker{[] { assert(current == nullptr); Scope unused{Phase::ShaderModule}; }};
    worker.join();
    exception.Finish();
    assert(exception.Nanoseconds()[size_t(Phase::MetadataRead)] == 20);
    assert(exception.Nanoseconds()[size_t(Phase::Other)] == 10);
    assert(current == nullptr);
    puts("PASS: exclusive nested phase totals, exception cleanup, idempotent stop and thread isolation");
}
