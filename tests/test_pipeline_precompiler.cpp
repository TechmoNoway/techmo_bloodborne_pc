// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/pipeline_precompiler.h"
#include "../engine/renderer/shim/bbport_write_epoch.h"
#include "video_core/renderer_vulkan/pipeline_retry.h"
#include "video_core/renderer_vulkan/graphics_scan_policy.h"
#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <crtdbg.h>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>
#include <vector>

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    using Vulkan::PipelinePrecompiler;
    for(unsigned total=1;total<512;++total) {
        unsigned count=0;
        for(unsigned i=0;i<total;++i) count+=Vulkan::GraphicsScan::Sample(i,total);
        assert(count==std::min(total,16u));
        assert(Vulkan::GraphicsScan::Sample(0,total) && Vulkan::GraphicsScan::Sample(total-1,total));
        assert(!Vulkan::GraphicsScan::Sample(total,total));
    }
    assert(Vulkan::GraphicsScan::Sample(0xfffffffeu,0xffffffffu));
    puts("PASS: graphics scanner samples head and final draw, spreads exactly 16 selections without overflow");

    {
        BbBackingWriteEpoch epochs;
        constexpr std::uint64_t address = 0x100ff000, size = 8192;
        constexpr auto unrelated = address + (16ull << 20);
        const auto original = epochs.Snapshot(address, size);
        const auto other = epochs.Snapshot(unrelated, size);
        { BbBackingWriteEpoch::Write write{epochs, unrelated, size};
          assert(epochs.Snapshot() == BbBackingWriteEpoch::Invalid);
          assert(epochs.Snapshot(address, size) == original);
          assert(epochs.Snapshot(unrelated, size) == BbBackingWriteEpoch::Invalid); }
        assert(epochs.Snapshot(address, size) == original);
        assert(epochs.Snapshot(unrelated, size) != other);
        std::promise<void> started, release;
        auto gate = release.get_future().share();
        std::thread writer{[&] {
            BbBackingWriteEpoch::Write write{epochs, address + 4096, 4};
            started.set_value(); gate.wait();
        }};
        started.get_future().wait();
        { BbBackingWriteEpoch::Write overlap{epochs, address + 4096, 4}; }
        for (unsigned i = 0; i < 1000; ++i) {
            assert(epochs.Snapshot(address, size) == BbBackingWriteEpoch::Invalid);
            assert(epochs.Snapshot(unrelated, size) != BbBackingWriteEpoch::Invalid);
        }
        release.set_value(); writer.join();
        const auto changed = epochs.Snapshot(address, size);
        assert(changed != original && changed != BbBackingWriteEpoch::Invalid);
        // Hash-stripe aliasing is allowed to invalidate extra ranges, never to miss a write.
        { BbBackingWriteEpoch::Write alias{epochs, address + (1024ull << 20), size};
          assert(epochs.Snapshot(address, size) == BbBackingWriteEpoch::Invalid); }
        assert(epochs.Snapshot(address, size) != changed);
        { BbBackingWriteEpoch::Write unknown{epochs};
          assert(epochs.Snapshot(address, size) == BbBackingWriteEpoch::Invalid); }
        { BbBackingWriteEpoch::Write large{epochs, unrelated, 65ull << 20};
          assert(epochs.Snapshot(address, size) == BbBackingWriteEpoch::Invalid); }
        { BbBackingWriteEpoch::Write overflow{epochs, BbBackingWriteEpoch::Invalid - 3, 8};
          assert(epochs.Snapshot(address, size) == BbBackingWriteEpoch::Invalid); }
        assert(epochs.Snapshot(address, size) != BbBackingWriteEpoch::Invalid);
        puts("PASS: ranged backing epochs ignore unrelated writes, catch boundary/overlapping "
             "writers, and conservatively cover alias/unknown/large/overflow ranges");
    }

    {
        PipelinePrecompiler<int,int> deferred{2};
        std::promise<void> started,release;
        auto gate=release.get_future().share();
        auto done=deferred.SubmitReserved(1,[&] { started.set_value();gate.wait();return 99; });
        started.get_future().wait();
        // Overlapping backing writes must never look quiescent.
        BbBackingWriteEpoch epoch;
        const auto initial=epoch.Snapshot();
        { BbBackingWriteEpoch::Write a{epoch};
          { BbBackingWriteEpoch::Write b{epoch};assert(epoch.Snapshot()==BbBackingWriteEpoch::Invalid); }
          assert(epoch.Snapshot()==BbBackingWriteEpoch::Invalid);
        }
        assert(epoch.Snapshot()!=initial && epoch.Snapshot()!=BbBackingWriteEpoch::Invalid);
        release.set_value();deferred.Stop();
        Vulkan::PipelineTiming timing;
        assert(done(&timing)==99 && timing.valid && timing.required);
        puts("PASS: deferred completion outlives shutdown, shares single compiler, overlapping backing epochs stay invalid");
    }
    {
        std::function<int(Vulkan::PipelineTiming*)> completion;
        { PipelinePrecompiler<int,int> gone{1}; completion=gone.SubmitReserved(7,[] { return 77; }); }
        Vulkan::PipelineTiming timing;
        assert(completion(&timing)==77 && timing.valid);
        PipelinePrecompiler<int,int> errors{1};
        auto failed=errors.SubmitReserved(1,[]() -> int { throw std::runtime_error("driver"); });
        bool caught=false;
        try { failed(&timing); } catch(const std::runtime_error&) { caught=true; }
        assert(caught && timing.valid);
        auto next=errors.SubmitReserved(2,[] { return 2; });assert(next(nullptr)==2);
        puts("PASS: completion survives compiler destruction and error releases bounded native job");
    }
    {
        PipelinePrecompiler<int,int> bounded{1};
        std::promise<void> started,release;
        auto gate=release.get_future().share();
        std::array<std::function<int(Vulkan::PipelineTiming*)>,4> completions;
        completions[0]=bounded.SubmitReserved(0,[&] { started.set_value();gate.wait();return 0; });
        started.get_future().wait();
        for(int i=1;i<4;++i) completions[i]=bounded.SubmitReserved(i,[i] { return i; });
        auto fifth=std::async(std::launch::async,[&] { return bounded.SubmitReserved(4,[] { return 4; }); });
        assert(fifth.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
        release.set_value();
        auto last=fifth.get();
        for(int i=0;i<4;++i) assert(completions[i](nullptr)==i);
        assert(last(nullptr)==4);
        puts("PASS: first-use native queue applies backpressure at four jobs and releases capacity");
    }
    {
        PipelinePrecompiler<int,std::unique_ptr<int>> early{2};
        std::promise<void> published,release;
        auto gate=release.get_future().share();std::atomic<bool> continuation_done{false};
        assert(early.QueueEarly(1,[&] { return [&](auto publish,auto) {
            publish(std::make_unique<int>(42));published.set_value();gate.wait();continuation_done=true;
        }; }));
        published.get_future().wait();
        Vulkan::PipelineTiming timing;
        auto result=early.TakeOrReserve(1,&timing);
        assert(result && **result==42 && timing.valid && !continuation_done);
        assert(early.Outstanding()==0);
        auto stop=std::async(std::launch::async,[&] { early.Stop(); });
        assert(stop.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
        release.set_value();stop.get();assert(continuation_done);
        PipelinePrecompiler<int,int> failures{1};
        assert(failures.QueueEarly(1,[] { return [](auto,auto) { throw std::runtime_error("frontend"); }; }));
        bool caught=false;try { failures.TakeOrReserve(1,&timing); } catch(const std::runtime_error&) { caught=true; }
        assert(caught && timing.valid && failures.Outstanding()==0);
        assert(failures.QueueEarly(2,[] { return [](auto,auto) {}; }));
        caught=false;try { failures.TakeOrReserve(2); } catch(const std::logic_error&) { caught=true; }
        assert(caught);
        puts("PASS: frontend future publishes before continuation, shutdown drains, timing/error/missing-publication remain safe");
    }
    {
        PipelinePrecompiler<int, int> yielding{4};
        std::promise<void> published, release;
        auto gate = release.get_future().share();
        std::vector<int> order;
        assert(yielding.QueueEarly(1, [&] { return [&](auto publish, auto) {
            publish(1); published.set_value(); gate.wait();
            yielding.Continue(1, [&] { order.push_back(1); });
        }; }));
        published.get_future().wait();
        auto required = yielding.SubmitReserved(2, [&] { order.push_back(2); return 2; });
        assert(yielding.Queue(3, [&] { return [&] { order.push_back(3); return 3; }; }));
        release.set_value();
        assert(required(nullptr) == 2);
        yielding.Stop();
        assert((order == std::vector<int>{2, 1, 3}));
        assert(yielding.TakeOrReserve(1) == 1 && yielding.TakeOrReserve(3) == 3);
        assert(yielding.Outstanding() == 0 && yielding.Queued() == 3);
        bool rejected = false;
        try { yielding.Continue(4, [] {}); } catch (const std::logic_error&) { rejected = true; }
        assert(rejected);
        puts("PASS: lookahead yields before native continuation to pending required work; no extra worker or speculative result slot");
    }
    {
        PipelinePrecompiler<int, int> draining{2};
        std::promise<void> published, release;
        auto gate = release.get_future().share();
        std::vector<int> order;
        assert(draining.QueueEarly(1, [&] { return [&](auto publish, auto) {
            publish(1); published.set_value(); gate.wait();
            draining.Continue(1, [&] { order.push_back(1); });
        }; }));
        published.get_future().wait();
        // The already adopted frontend's continuation is now required itself.
        assert(draining.TakeOrReserve(1) == 1);
        auto later = draining.SubmitReserved(2, [&] { order.push_back(2); return 2; });
        auto stopped = std::async(std::launch::async, [&] { draining.Stop(); });
        assert(stopped.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
        release.set_value(); stopped.get();
        assert(later(nullptr) == 2 && (order == std::vector<int>{1, 2}));
        puts("PASS: adopted frontend continuation stays urgent and survives shutdown while its frontend is running");
    }
    PipelinePrecompiler<int, std::unique_ptr<int>> compiler{2};
    std::promise<void> release, started;
    auto gate = release.get_future().share();
    std::atomic<int> builds{0}, snapshots{0};
    assert(compiler.Queue(1, [&] {
        ++snapshots;
        return [&] {
            ++builds;
            started.set_value();
            gate.wait();
            return std::make_unique<int>(42);
        };
    }));
    started.get_future().wait();
    assert(!compiler.Queue(1, [&] {
        ++snapshots;
        return [] { return std::make_unique<int>(0); };
    }));
    assert(compiler.Queue(2, [&] {
        ++snapshots;
        return [&] {
            ++builds;
            return std::make_unique<int>(43);
        };
    }));
    assert(!compiler.Queue(3, [&] {
        ++snapshots;
        return [] { return std::make_unique<int>(0); };
    }));
    assert(snapshots == 2 && compiler.Outstanding() == 2);
    Vulkan::PipelineTiming first_trace;
    auto required = std::async(std::launch::async, [&] { return compiler.TakeOrReserve(1, &first_trace); });
    assert(required.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    // Required draws wait for completion, rather than accepting an incomplete
    // result.
    release.set_value();
    auto result = required.get();
    assert(result && **result == 42);
    assert(first_trace.valid && !first_trace.required && first_trace.queued <= first_trace.started &&
           first_trace.started <= first_trace.needed && first_trace.needed <= first_trace.finished);
    auto second = compiler.TakeOrReserve(2);
    assert(second && **second == 43);
    assert(builds == 2 && compiler.Outstanding() == 0);
    assert(!compiler.Queue(1, [] { return [] { return std::make_unique<int>(0); }; }));
    assert(!compiler.TakeOrReserve(4)); // Reserve foreground ownership before building.
    assert(!compiler.Queue(4, [] { return [] { return std::make_unique<int>(0); }; }));
    compiler.Known(5);
    assert(!compiler.Queue(5, [] { return [] { return std::make_unique<int>(0); }; }));
    assert(compiler.Queue(6, [] {
        return []() -> std::unique_ptr<int> { throw std::runtime_error("driver rejected"); };
    }));
    bool failed = false;
    try {
        (void)compiler.TakeOrReserve(6);
    } catch (const std::runtime_error &) {
        failed = true;
    }
    assert(failed && compiler.Outstanding() == 0);
    bool snapshot_failed = false;
    try {
        compiler.Queue(8,
                       []() -> std::function<std::unique_ptr<int>()> { throw std::bad_alloc{}; });
    } catch (const std::bad_alloc &) {
        snapshot_failed = true;
    }
    assert(snapshot_failed && compiler.Outstanding() == 0);
    assert(compiler.Queue(8, [] { return [] { return std::make_unique<int>(8); }; }));
    auto retried = compiler.TakeOrReserve(8);
    assert(retried && **retried == 8);
    compiler.Stop();
    assert(!compiler.Queue(7, [] { return [] { return std::make_unique<int>(0); }; }));
    puts("PASS: bounded snapshots/results, duplicate prevention, foreground "
         "reservation, required waits, exception fallback and shutdown drain");

    PipelinePrecompiler<int, std::unique_ptr<int>> concurrent{8};
    std::atomic<int> accepted{0}, made{0};
    std::vector<std::thread> producers;
    for (int i = 0; i < 8; ++i)
        producers.emplace_back([&] {
            for (int key = 0; key < 8; ++key) {
                accepted += concurrent.Queue(key, [&, key] {
                    return [&, key] {
                        ++made;
                        return std::make_unique<int>(key);
                    };
                });
            }
        });
    for (auto &producer : producers)
        producer.join();
    assert(accepted == 8 && concurrent.Outstanding() == 8);
    concurrent.Stop(); // All accepted work is finished, even when no draw has
                       // consumed it.
    for (int key = 0; key < 8; ++key) {
        auto value = concurrent.TakeOrReserve(key);
        assert(value && **value == key);
    }
    assert(made == 8 && concurrent.Outstanding() == 0);
    puts("PASS: eight concurrent producers create each key once; completed "
         "unused results remain bounded");

    PipelinePrecompiler<int, int> priority{3};
    std::promise<void> first_started, release_first, release_second;
    auto first_gate = release_first.get_future().share();
    auto second_gate = release_second.get_future().share();
    assert(priority.Queue(1, [&] {
        return [&] {
            first_started.set_value();
            first_gate.wait();
            return 1;
        };
    }));
    first_started.get_future().wait();
    assert(priority.Queue(2, [&] {
        return [&] {
            second_gate.wait();
            return 2;
        };
    }));
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

    PipelinePrecompiler<int, int> misses{2};
    std::promise<void> miss_started, release_miss, release_speculative, required_snapshot;
    auto miss_gate = release_miss.get_future().share();
    auto speculative_gate = release_speculative.get_future().share();
    assert(misses.Queue(1, [&] {
        return [&] {
            miss_started.set_value();
            miss_gate.wait();
            return 1;
        };
    }));
    miss_started.get_future().wait();
    assert(misses.Queue(2, [&] {
        return [&] {
            speculative_gate.wait();
            return 2;
        };
    }));
    std::thread::id requester, compiler_thread;
    auto miss = std::async(std::launch::async, [&] {
        requester = std::this_thread::get_id();
        return misses.TakeOrQueue(3, [&] {
            required_snapshot.set_value();
            return [&] {
                compiler_thread = std::this_thread::get_id();
                return 3;
            };
        });
    });
    required_snapshot.get_future().wait();
    assert(misses.Outstanding() == 3 && misses.RequiredQueued() == 1);
    assert(!misses.Queue(4, [] { return [] { return 4; }; }));
    release_miss.set_value();
    const bool required_promoted =
        miss.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release_speculative.set_value();
    auto miss_result = miss.get();
    misses.Stop();
    assert(required_promoted && miss_result == 3 && compiler_thread != requester);
    bool duplicated = false;
    auto retained = misses.TakeOrQueue(2, [&] {
        duplicated = true;
        return [] { return 0; };
    });
    assert(retained == 2 && !duplicated);
    assert(!misses.TakeOrQueue(4, [] { return [] { return 4; }; }));
    assert(misses.TakeOrReserve(1) == 1 && misses.Outstanding() == 0);
    puts("PASS: required misses run on worker, exceed speculative capacity by "
         "one, overtake queued work and drain after stop without duplicate "
         "snapshots");

    std::atomic<int> priority_boosts{0}, priority_restores{0};
    PipelinePrecompiler<int, int> errors{
        1, {}, [&](bool waiting) { ++(waiting ? priority_boosts : priority_restores); }};
    Vulkan::PipelineTiming failed_trace;
    bool required_failed = false;
    try {
        errors.TakeOrQueue(
            1, [] { return []() -> int { throw std::runtime_error{"required compile"}; }; }, &failed_trace);
    } catch (const std::runtime_error &) {
        required_failed = true;
    }
    assert(required_failed && errors.Outstanding() == 0);
    assert(failed_trace.valid && failed_trace.required && failed_trace.needed <= failed_trace.queued &&
           failed_trace.queued <= failed_trace.started && failed_trace.started <= failed_trace.finished);
    bool required_snapshot_failed = false;
    try {
        errors.TakeOrQueue(2, []() -> std::function<int()> { throw std::bad_alloc{}; });
    } catch (const std::bad_alloc &) {
        required_snapshot_failed = true;
    }
    assert(required_snapshot_failed && errors.Outstanding() == 0);
    assert(errors.TakeOrQueue(2, [] { return [] { return 2; }; }) == 2);
    assert(priority_boosts == 2 && priority_restores == 2);
    puts("PASS: required driver and snapshot failures retire ownership without "
         "hanging or losing retry capacity");

    Vulkan::PipelineRetry retry;
    assert(retry.Claimable(0, false));
    retry.Claim(0);
    assert(!retry.Claimable(1, true));
    retry.Complete(true);
    assert(!retry.Claimable(0, true) && !retry.Claimable(1, false));
    assert(retry.Claimable(1, true));
    retry.Claim(1);
    // Generation 2 was published while this pass was running.
    retry.Complete(true);
    assert(retry.Claimable(2, true));
    for (unsigned pass = 2; pass < Vulkan::PipelineRetry::MaxPasses; ++pass) {
        retry.Claim(pass);
        retry.Complete(true);
    }
    assert(!retry.Claimable(99, true));
    Vulkan::PipelineRetry ready;
    ready.Claim(0);
    ready.Complete(false);
    assert(!ready.Claimable(1, true));
    puts("PASS: unavailable draws retry only on shader publications, retain "
         "publications during work, stop at budget and never repeat ready data");
}
