// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace Vulkan {
// Lookahead work is bounded until consumed, including completed results. The
// foreground reserves an absent key before building it synchronously, so a
// concurrent lookahead cannot start a duplicate. Required work is never skipped.
template <typename Key, typename Result, typename Hash = std::hash<Key>>
class PipelinePrecompiler {
public:
    PipelinePrecompiler(size_t limit, std::function<void()> start = {}) : limit{limit} {
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
        auto entry = keys.emplace(key, task.get_future()).first;
        try {
            tasks.push_back({key, std::move(task)});
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
    std::optional<Result> TakeOrReserve(const Key& key) {
        std::future<Result> future;
        {
            std::scoped_lock lock{mutex};
            auto [it, inserted] = keys.try_emplace(key);
            if (inserted || !it->second.valid()) return std::nullopt;
            future = std::move(it->second);
            // A required draw must not wait behind all speculative queued jobs.
            // Driver calls already running cannot be preempted; promote work that
            // has not started before waiting for its future.
            const auto pending = std::find_if(tasks.begin(), tasks.end(),
                                              [&](const Job& job) { return job.key == key; });
            if (pending != tasks.end())
                std::rotate(tasks.begin(), pending, std::next(pending));
        }
        struct Retire {
            PipelinePrecompiler* owner;
            ~Retire() { std::scoped_lock lock{owner->mutex}; --owner->outstanding; }
        } retire{this};
        return future.get();
    }

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
    size_t Outstanding() const { std::scoped_lock lock{mutex}; return outstanding; }

private:
    struct Job {
        Key key;
        std::packaged_task<Result()> task;
    };
    void Run() {
        for (;;) {
            std::packaged_task<Result()> task;
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
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool accepting = true;
    size_t outstanding = 0, queued = 0;
    std::unordered_map<Key, std::future<Result>, Hash> keys;
    std::deque<Job> tasks;
    std::thread worker;
};
} // namespace Vulkan
