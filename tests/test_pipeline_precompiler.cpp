// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>
#include <vector>
#include <crtdbg.h>
#include "video_core/renderer_vulkan/pipeline_precompiler.h"

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    using Vulkan::PipelinePrecompiler;
    PipelinePrecompiler<int, std::unique_ptr<int>> compiler{2};
    std::promise<void> release, started;
    auto gate = release.get_future().share();
    std::atomic<int> builds{0}, snapshots{0};
    assert(compiler.Queue(1, [&] {
        ++snapshots;
        return [&] { ++builds; started.set_value(); gate.wait(); return std::make_unique<int>(42); };
    }));
    started.get_future().wait();
    assert(!compiler.Queue(1, [&] { ++snapshots; return [] { return std::make_unique<int>(0); }; }));
    assert(compiler.Queue(2, [&] {
        ++snapshots; return [&] { ++builds; return std::make_unique<int>(43); };
    }));
    assert(!compiler.Queue(3, [&] { ++snapshots; return [] { return std::make_unique<int>(0); }; }));
    assert(snapshots == 2 && compiler.Outstanding() == 2);
    auto required = std::async(std::launch::async, [&] { return compiler.TakeOrReserve(1); });
    assert(required.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    // Required draws wait for completion, rather than accepting an incomplete result.
    release.set_value();
    auto result = required.get(); assert(result && **result == 42);
    auto second = compiler.TakeOrReserve(2); assert(second && **second == 43);
    assert(builds == 2 && compiler.Outstanding() == 0);
    assert(!compiler.Queue(1, [] { return [] { return std::make_unique<int>(0); }; }));
    assert(!compiler.TakeOrReserve(4)); // Reserve foreground ownership before building.
    assert(!compiler.Queue(4, [] { return [] { return std::make_unique<int>(0); }; }));
    compiler.Known(5);
    assert(!compiler.Queue(5, [] { return [] { return std::make_unique<int>(0); }; }));
    assert(compiler.Queue(6, [] { return []() -> std::unique_ptr<int> { throw std::runtime_error("driver rejected"); }; }));
    bool failed = false;
    try { (void)compiler.TakeOrReserve(6); } catch (const std::runtime_error&) { failed = true; }
    assert(failed && compiler.Outstanding() == 0);
    bool snapshot_failed = false;
    try {
        compiler.Queue(8, []() -> std::function<std::unique_ptr<int>()> { throw std::bad_alloc{}; });
    } catch (const std::bad_alloc&) { snapshot_failed = true; }
    assert(snapshot_failed && compiler.Outstanding() == 0);
    assert(compiler.Queue(8, [] { return [] { return std::make_unique<int>(8); }; }));
    auto retried = compiler.TakeOrReserve(8); assert(retried && **retried == 8);
    compiler.Stop();
    assert(!compiler.Queue(7, [] { return [] { return std::make_unique<int>(0); }; }));
    puts("PASS: bounded snapshots/results, duplicate prevention, foreground reservation, required waits, exception fallback and shutdown drain");

    PipelinePrecompiler<int, std::unique_ptr<int>> concurrent{8};
    std::atomic<int> accepted{0}, made{0};
    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i) producers.emplace_back([&] {
        for (int key = 0; key < 8; ++key) {
            accepted += concurrent.Queue(key, [&, key] {
                return [&, key] { ++made; return std::make_unique<int>(key); };
            });
        }
    });
    for (auto& producer : producers) producer.join();
    assert(accepted == 8 && concurrent.Outstanding() == 8);
    concurrent.Stop(); // All accepted work is finished, even when no draw has consumed it.
    for (int key = 0; key < 8; ++key) {
        auto value = concurrent.TakeOrReserve(key); assert(value && **value == key);
    }
    assert(made == 8 && concurrent.Outstanding() == 0);
    puts("PASS: eight concurrent producers create each key once; completed unused results remain bounded");

    PipelinePrecompiler<int, int> priority{3};
    std::promise<void> first_started, release_first, release_second;
    auto first_gate = release_first.get_future().share();
    auto second_gate = release_second.get_future().share();
    assert(priority.Queue(1, [&] { return [&] { first_started.set_value(); first_gate.wait(); return 1; }; }));
    first_started.get_future().wait();
    assert(priority.Queue(2, [&] { return [&] { second_gate.wait(); return 2; }; }));
    assert(priority.Queue(3, [] { return [] { return 3; }; }));
    auto needed = std::async(std::launch::async, [&] { return priority.TakeOrReserve(3); });
    assert(needed.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    release_first.set_value();
    const bool promoted = needed.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release_second.set_value(); // Cleanup also completes if promotion regresses.
    auto third = needed.get();
    priority.Stop();
    assert(promoted && third && *third == 3);
    puts("PASS: required pipeline overtakes unstarted speculative jobs");
}
