// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Vulkan::ReplayMetrics {
enum class Phase : size_t {
    RecordDecode, MetadataRead, MetadataDecode, SpirvRead, SpirvValidate,
    ShaderModule, DescriptorLayout, PipelineLayout, PipelineState, NativePipeline,
    Other, Count
};
inline constexpr std::array<std::string_view, size_t(Phase::Count)> Names{
    "record_decode", "metadata_read", "metadata_decode", "spirv_read", "spirv_validate",
    "shader_module", "descriptor_layout", "pipeline_layout", "pipeline_state", "native_pipeline",
    "other"};
class Capture;
inline thread_local Capture* current = nullptr;

// Opt-in on the lab thread only. Inactive scopes never read the clock. Nested
// phases account exclusive intervals, so their sum equals the captured duration.
class Capture {
public:
    using Clock = uint64_t (*)();
    explicit Capture(Clock clock_ = Now) : clock{clock_} {}
    ~Capture() { Finish(); }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    void Start() {
        if (running) return;
        previous = current;
        current = this;
        phase = Phase::Other;
        last = clock();
        running = true;
    }
    void Finish() {
        if (!running) return;
        Account();
        current = previous;
        running = false;
    }
    Phase Switch(Phase next) {
        Account();
        const auto old = phase;
        phase = next;
        return old;
    }
    const auto& Nanoseconds() const { return elapsed; }
private:
    static uint64_t Now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void Account() {
        const auto now = clock();
        elapsed[size_t(phase)] += now - last;
        last = now;
    }
    std::array<uint64_t, size_t(Phase::Count)> elapsed{};
    Clock clock;
    Capture* previous{};
    Phase phase{Phase::Other};
    uint64_t last{};
    bool running{};
};
class Scope {
public:
    explicit Scope(Phase phase) : capture{current} {
        if (capture) previous = capture->Switch(phase);
    }
    ~Scope() { Stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Stop() {
        if (capture) {
            capture->Switch(previous);
            capture = nullptr;
        }
    }
private:
    Capture* capture{};
    Phase previous{Phase::Other};
};
}
