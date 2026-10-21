// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>

namespace Vulkan {
struct PipelineTiming {
    using Clock = std::chrono::steady_clock;
    inline static const auto epoch = Clock::now();
    Clock::time_point queued{}, started{}, finished{}, needed{};
    double capacity_wait_ms = 0, admission_wait_ms = 0;
    bool required = false;
    bool valid = false;
    static double Ms(Clock::duration value) {
        return std::chrono::duration<double, std::milli>(value).count();
    }
};
// Lookahead work is bounded until consumed, including completed results. The
// foreground reserves an absent key before building it synchronously, so a
// concurrent lookahead cannot start a duplicate. Required work is never skipped.
template <typename Key, typename Result, typename Hash = std::hash<Key>>
class PipelinePrecompiler {
public:
    PipelinePrecompiler(size_t limit, std::function<void()> start = {},
                        std::function<void(bool)> wait_priority = {})
        : limit{limit}, wait_priority{std::move(wait_priority)} {
        worker = std::thread([this, start = std::move(start)] {
            if (start) start();
            Run();
        });
    }
    ~PipelinePrecompiler() { Stop(); }
    PipelinePrecompiler(const PipelinePrecompiler&) = delete;
    PipelinePrecompiler& operator=(const PipelinePrecompiler&) = delete;

    // make_work snapshots inputs only after capacity and duplicate checks pass.
    template <typename Prepare>
    bool Queue(const Key& key, Prepare&& make_work) {
        std::scoped_lock lock{mutex};
        if (!accepting || keys.contains(key) || outstanding >= limit) return false;
        auto work = make_work();
        std::packaged_task<Result()> task{std::move(work)};
        auto timing = std::make_shared<PipelineTiming>();
        timing->queued = PipelineTiming::Clock::now();
        task = TimedTask(std::move(task), timing);
        auto entry = keys.emplace(key, Entry{task.get_future(), timing}).first;
        try {
            tasks.push_back({key, AsJob(std::move(task))});
        } catch (...) {
            keys.erase(entry);
            throw;
        }
        ++outstanding; ++queued;
        cv.notify_one();
        return true;
    }

    // An empty result reserves the synchronous path. Waiting occurs without the
    // registry lock; other preparation workers can still enqueue unrelated keys.
    std::optional<Result> TakeOrReserve(const Key& key, PipelineTiming* trace = nullptr) {
        std::future<Result> future;
        std::shared_ptr<PipelineTiming> timing;
        const auto needed = PipelineTiming::Clock::now();
        if (trace) *trace = {};
        {
            std::scoped_lock lock{mutex};
            auto [it, inserted] = keys.try_emplace(key);
            if (inserted || !it->second.future.valid()) return std::nullopt;
            future = std::move(it->second.future);
            timing = std::move(it->second.timing);
            // A required draw must not wait behind all speculative queued jobs.
            // Driver calls already running cannot be preempted; promote work that
            // has not started before waiting for its future.
            const auto pending = std::find_if(tasks.begin(), tasks.end(),
                                              [&](const Job& job) { return job.key == key; });
            if (pending != tasks.end()) {
                auto before = tasks.begin();
                while (before != pending && before->required) ++before;
                pending->required = true;
                std::rotate(before, pending, std::next(pending));
            }
        }
        struct Retire {
            PipelinePrecompiler* owner;
            ~Retire() { owner->SetWaiting(false); std::scoped_lock lock{owner->mutex}; --owner->outstanding; }
        } retire{this};
        SetWaiting(true);
        return Consume(future, timing, needed, trace);
    }

    // The single GPU consumer can submit a required miss even when speculative
    // capacity is full. At most one extra result exists while that consumer
    // waits. This keeps native driver compilation off the GPU command thread
    // without skipping its draw. Snapshotting remains on the requesting thread.
    template <typename Prepare>
    std::optional<Result> TakeOrQueue(const Key& key, Prepare&& make_work,
                                      PipelineTiming* trace = nullptr) {
        std::future<Result> future;
        std::shared_ptr<PipelineTiming> timing;
        const auto needed = PipelineTiming::Clock::now();
        if (trace) *trace = {};
        {
            std::scoped_lock lock{mutex};
            auto it = keys.find(key);
            if (it == keys.end()) {
                if (!accepting) return std::nullopt;
                auto work = make_work();
                std::packaged_task<Result()> task{std::move(work)};
                timing = std::make_shared<PipelineTiming>();
                timing->required = true;
                timing->queued = PipelineTiming::Clock::now();
                task = TimedTask(std::move(task), timing);
                it = keys.emplace(key, Entry{task.get_future(), timing}).first;
                try {
                    const auto before = std::find_if(tasks.begin(), tasks.end(), [](const Job& job) { return !job.required; });
                    tasks.insert(before, {key, AsJob(std::move(task)), true});
                } catch (...) {
                    keys.erase(it);
                    throw;
                }
                ++outstanding; ++queued; ++required_queued;
            }
            if (!it->second.future.valid()) return std::nullopt;
            future = std::move(it->second.future);
            timing = std::move(it->second.timing);
            const auto pending = std::find_if(tasks.begin(), tasks.end(),
                                              [&](const Job& job) { return job.key == key; });
            if (pending != tasks.end()) {
                auto before = tasks.begin();
                while (before != pending && before->required) ++before;
                pending->required = true;
                std::rotate(before, pending, std::next(pending));
            }
        }
        cv.notify_one();
        struct Retire {
            PipelinePrecompiler* owner;
            ~Retire() { owner->SetWaiting(false); std::scoped_lock lock{owner->mutex}; --owner->outstanding; }
        } retire{this};
        SetWaiting(true);
        return Consume(future, timing, needed, trace);
    }

    // Reserved first-use native jobs use the SAME compiler thread. Limit both
    // queued and running jobs to four; finished handles belong to normal pipeline
    // objects, not speculative cache entries. No consumer or guest pointer is
    // captured by the task. The returned completion can outlive Stop().
    template <typename Work>
    std::function<Result(PipelineTiming*)> SubmitReserved(const Key& key, Work work) {
        const auto requested = PipelineTiming::Clock::now();
        std::unique_lock lock{mutex};
        const auto admitted = PipelineTiming::Clock::now();
        cv.wait(lock, [&] { return !accepting || deferred_jobs < 4; });
        const auto capacity_ready = PipelineTiming::Clock::now();
        if (!accepting) throw std::runtime_error{"pipeline compiler stopped"};
        auto timing = std::make_shared<PipelineTiming>();
        timing->queued = PipelineTiming::Clock::now();
        timing->admission_wait_ms = PipelineTiming::Ms(admitted - requested);
        timing->capacity_wait_ms = PipelineTiming::Ms(capacity_ready - admitted);
        timing->required = true;
        std::packaged_task<Result()> task{[this, work = std::move(work)]() mutable {
            struct Retire {
                PipelinePrecompiler* owner;
                ~Retire() {
                    { std::scoped_lock lock{owner->mutex}; --owner->deferred_jobs; }
                    owner->cv.notify_all();
                }
            } retire{this};
            return work();
        }};
        task = TimedTask(std::move(task), timing);
        auto future = std::make_shared<std::future<Result>>(task.get_future());
        auto waiting = wait_priority;
        // Allocate the completion before publishing a task: allocation failure
        // cannot leave a live job referring to a partially constructed pipeline.
        std::function<Result(PipelineTiming*)> completion = [future, timing, waiting](PipelineTiming* trace) {
            struct Priority {
                const std::function<void(bool)>& callback;
                explicit Priority(const std::function<void(bool)>& cb) : callback{cb} {
                    if (callback) try { callback(true); } catch (...) {}
                }
                ~Priority() { if (callback) try { callback(false); } catch (...) {} }
            } priority{waiting};
            return Consume(*future, timing, PipelineTiming::Clock::now(), trace);
        };
        // Required work precedes speculation, but preserves first-use FIFO.
        const auto before = std::find_if(tasks.begin(), tasks.end(), [](const Job& job) { return !job.required; });
        tasks.insert(before, Job{key, AsJob(std::move(task)), true});
        ++deferred_jobs; ++queued; ++required_queued;
        lock.unlock(); cv.notify_one();
        return completion;
    }

    // Only a running frontend may yield its native continuation. This creates
    // no extra compiler/cache and accepts continuations even during Stop's drain.
    // Put it ahead of other speculation, but behind already required work. If
    // its frontend has been consumed, its native work is itself now required.
    template <typename Work>
    void Continue(const Key& key, Work work) {
        if (std::this_thread::get_id() != worker.get_id())
            throw std::logic_error{"pipeline continuation must originate on its compiler"};
        std::packaged_task<void()> task{std::move(work)};
        std::scoped_lock lock{mutex};
        const auto entry = keys.find(key);
        const bool needed = entry != keys.end() && !entry->second.future.valid();
        const auto before = needed ? tasks.begin() :
            std::find_if(tasks.begin(), tasks.end(), [](const Job& job) { return !job.required; });
        tasks.insert(before, {key, std::move(task), needed});
    }

    using Publish = std::function<void(Result)>;
    using EarlyWork = std::function<void(Publish, std::shared_ptr<PipelineTiming>)>;
    template <typename Prepare>
    bool QueueEarly(const Key& key, Prepare&& make_work) {
        static_assert(std::is_nothrow_move_constructible_v<Result>, "frontend publication must transfer ownership without allocation");
        std::scoped_lock lock{mutex};
        if (!accepting || keys.contains(key) || outstanding >= limit) return false;
        auto work = make_work();
        auto ready = std::make_shared<std::promise<Result>>();
        auto timing = std::make_shared<PipelineTiming>();
        timing->queued = PipelineTiming::Clock::now();
        auto entry = keys.emplace(key, Entry{ready->get_future(), timing}).first;
        try {
            std::packaged_task<void()> task{[work = std::move(work), ready, timing]() mutable {
                timing->started = PipelineTiming::Clock::now();
                bool published = false;
                try {
                    work([&](Result value) {
                        if (published) throw std::logic_error{"duplicate pipeline frontend publication"};
                        timing->finished = PipelineTiming::Clock::now();
                        // Result is immutable from this point. Native completion
                        // has its own promise/timing, never these shared fields.
                        ready->set_value(std::move(value));
                        published = true;
                    }, timing);
                    if (!published) throw std::logic_error{"missing pipeline frontend publication"};
                } catch (...) {
                    if (!published) {
                        timing->finished = PipelineTiming::Clock::now();
                        ready->set_exception(std::current_exception());
                    }
                    // Accepted native continuation errors are delivered through
                    // the pipeline's native promise by EarlyWork itself.
                }
            }};
            tasks.push_back({key, std::move(task)});
        } catch (...) { keys.erase(entry); throw; }
        ++outstanding; ++queued;
        cv.notify_one();
        return true;
    }
    std::function<void(bool)> PriorityCallback() const { return wait_priority; }

    void Known(const Key& key) {
        std::scoped_lock lock{mutex};
        keys.try_emplace(key);
    }
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            accepting = false;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join(); // Drain accepted jobs before dependencies die.
    }
    size_t Queued() const { std::scoped_lock lock{mutex}; return queued; }
    size_t RequiredQueued() const { std::scoped_lock lock{mutex}; return required_queued; }
    size_t Outstanding() const { std::scoped_lock lock{mutex}; return outstanding; }

private:
    // Record completion inside the packaged task, before its future becomes ready,
    // including exceptions. The consumer reads these fields only after get().
    static std::packaged_task<Result()> TimedTask(std::packaged_task<Result()> task,
                                                 std::shared_ptr<PipelineTiming> timing) {
        auto future = task.get_future();
        return std::packaged_task<Result()>{[task = std::move(task), future = std::move(future),
                                             timing = std::move(timing)]() mutable {
            timing->started = PipelineTiming::Clock::now();
            struct Finish {
                PipelineTiming& value;
                ~Finish() { value.finished = PipelineTiming::Clock::now(); }
            } finish{*timing};
            task();
            return future.get();
        }};
    }
    static Result Consume(std::future<Result>& future,
                          const std::shared_ptr<PipelineTiming>& timing,
                          PipelineTiming::Clock::time_point needed, PipelineTiming* trace) {
        struct Publish {
            const std::shared_ptr<PipelineTiming>& timing;
            PipelineTiming::Clock::time_point needed;
            PipelineTiming* trace;
            ~Publish() {
                if (trace && timing) {
                    *trace = *timing;
                    trace->needed = needed;
                    trace->valid = true;
                }
            }
        } publish{timing, needed, trace};
        return future.get();
    }
    void SetWaiting(bool waiting) noexcept {
        // Scheduling is optional and cannot invalidate an accepted future.
        if (wait_priority) try { wait_priority(waiting); } catch (...) {}
    }
    static std::packaged_task<void()> AsJob(std::packaged_task<Result()> task) {
        return std::packaged_task<void()>{[task = std::move(task)]() mutable { task(); }};
    }
    struct Job {
        Key key;
        std::packaged_task<void()> task;
        bool required = false;
    };
    void Run() {
        for (;;) {
            std::packaged_task<void()> task;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [&] { return !accepting || !tasks.empty(); });
                if (tasks.empty()) return;
                task = std::move(tasks.front().task); tasks.pop_front();
            }
            task(); // packaged_task retains exceptions for the foreground fallback.
        }
    }
    const size_t limit;
    std::function<void(bool)> wait_priority;
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool accepting = true;
    size_t outstanding = 0, queued = 0, required_queued = 0, deferred_jobs = 0;
    struct Entry {
        std::future<Result> future;
        std::shared_ptr<PipelineTiming> timing;
    };
    std::unordered_map<Key, Entry, Hash> keys;
    std::deque<Job> tasks;
    std::thread worker;
};
} // namespace Vulkan
