// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_copy.h"
#include "bbport_threads.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bbport_toggles.h"
#include "common/thread.h"

namespace BbCopy {
namespace {

thread_local bool in_copy_thread = false;

struct FaultRequest {
    void* context;
    void* address;
    bool (*handler)(void*, void*);
    FaultRequest* next = nullptr;
    bool handled = false;
    std::atomic<bool> complete{false};
};

// Only the synchronous job's caller drains this mailbox. Workers merely publish
// stack-resident requests and wait; they never acquire the renderer's cache locks.
struct FaultMailbox {
    const std::thread::id owner = std::this_thread::get_id();
    std::atomic<FaultRequest*> pending{nullptr};
    void Drain() {
        if (std::this_thread::get_id() != owner) return;
        auto* request = pending.exchange(nullptr, std::memory_order_acquire);
        while (request) {
            auto* next = request->next;
            request->handled = request->handler(request->context, request->address);
            request->complete.store(true, std::memory_order_release);
            request = next; // The worker can discard the completed stack request now.
        }
    }
};
thread_local FaultMailbox* fault_mailbox = nullptr;

struct CopyScope {
    bool previous_copy = in_copy_thread;
    FaultMailbox* previous_mailbox = fault_mailbox;
    explicit CopyScope(FaultMailbox* mailbox) {
        in_copy_thread = true;
        fault_mailbox = mailbox;
    }
    ~CopyScope() {
        fault_mailbox = previous_mailbox;
        in_copy_thread = previous_copy;
    }
};

class Pool {
public:
    Pool() {
        // Copies are on the critical path (fences wait for them): normal priority, a quarter
        // of the hardware threads available to the process (Steam Deck 2).
        unsigned count = std::clamp(BbThreads::Available() / 4, 1u, 4u);
        if (const char* env = std::getenv("BB_COPY_THREADS")) {
            count = static_cast<unsigned>(std::clamp(std::atoi(env), 0, 16));
        }
        epochs.emplace_back();
        for (unsigned i = 0; i < count; ++i) {
            threads.emplace_back([this, i] { Loop(i); });
        }
    }
    ~Pool() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& thread : threads) {
            thread.join();
        }
    }

    bool Enabled() const {
        return !threads.empty();
    }

    void Async(std::function<void()> task) {
        {
            std::scoped_lock lk{mutex};
            const u64 epoch = epoch_base + epochs.size() - 1;
            ++epochs.back().pending;
            ++pending;
            async_tasks.push_back({std::move(task), epoch});
        }
        cv.notify_one();
    }

    void AfterCopies(std::function<void()> callback) {
        {
            std::unique_lock lk{mutex};
            if (epochs.size() > 1 || epochs.back().pending != 0 || draining) {
                // Runs once every copy issued so far is done, in order with earlier ones.
                epochs.back().callbacks.push_back(std::move(callback));
                epochs.emplace_back();
                return;
            }
        }
        callback();
    }

    void WaitAll() {
        // The waiting thread helps instead of only spinning.
        while (true) {
            Task task;
            {
                std::scoped_lock lk{mutex};
                if (pending == 0 && epochs.size() == 1 && !draining) {
                    return;
                }
                if (!async_tasks.empty()) {
                    task = std::move(async_tasks.front());
                    async_tasks.pop_front();
                }
            }
            if (task.run) {
                RunAsync(task);
            } else {
                std::this_thread::yield();
            }
        }
    }

    void Run(std::size_t count, const std::function<void(std::size_t)>& task, bool relay_faults) {
        auto job = std::make_shared<Job>();
        job->task = &task;
        job->count = count;
        job->relay_faults = relay_faults;
        {
            std::scoped_lock lk{mutex};
            current = job;
            ++generation;
        }
        cv.notify_all();
        Work(*job);
        // Workers only call the task for indices below count, all done before this returns.
        while (job->done.load(std::memory_order_acquire) < count) {
            if (job->relay_faults) job->faults.Drain();
            std::this_thread::yield();
        }
        std::scoped_lock lk{mutex};
        if (current == job) {
            current.reset();
        }
    }

private:
    using u64 = unsigned long long;
    struct Task {
        std::function<void()> run;
        u64 epoch{};
    };
    /// Copies issued between two AfterCopies() calls, and the callbacks waiting for them.
    struct Epoch {
        std::size_t pending = 0;
        std::vector<std::function<void()>> callbacks;
    };
    struct Job {
        const std::function<void(std::size_t)>* task{};
        std::size_t count{};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        bool relay_faults = false;
        FaultMailbox faults;
    };

    void RunAsync(Task& task) {
        {
            CopyScope scope{nullptr};
            task.run();
        }
        Complete(task.epoch);
    }

    /// Retires a task and runs the callbacks of epochs with nothing left, oldest first. One
    /// thread drains at a time so the callbacks keep their order.
    void Complete(u64 epoch) {
        std::unique_lock lk{mutex};
        --epochs[epoch - epoch_base].pending;
        --pending;
        if (draining) {
            return;
        }
        draining = true;
        while (true) {
            std::vector<std::function<void()>> ready;
            while (epochs.size() > 1 && epochs.front().pending == 0) {
                for (auto& callback : epochs.front().callbacks) {
                    ready.push_back(std::move(callback));
                }
                epochs.pop_front();
                ++epoch_base;
            }
            if (ready.empty()) {
                break;
            }
            lk.unlock();
            for (auto& callback : ready) {
                callback();
            }
            lk.lock();
        }
        draining = false;
    }

    static void Work(Job& job) {
        const bool owner = std::this_thread::get_id() == job.faults.owner;
        CopyScope scope{job.relay_faults && !owner ? &job.faults : nullptr};
        for (std::size_t i; (i = job.next.fetch_add(1, std::memory_order_relaxed)) < job.count;) {
            if (job.relay_faults && owner) job.faults.Drain();
            (*job.task)(i);
            job.done.fetch_add(1, std::memory_order_release);
        }
    }

    void Loop(unsigned index) {
        Common::SetCurrentThreadName(("bb:Copy" + std::to_string(index)).c_str());
        u64 seen = 0;
        std::unique_lock lk{mutex};
        while (true) {
            cv.wait(lk, [&] {
                return stop || !async_tasks.empty() || (generation != seen && current);
            });
            if (stop) {
                return;
            }
            if (!async_tasks.empty()) {
                Task task = std::move(async_tasks.front());
                async_tasks.pop_front();
                lk.unlock();
                RunAsync(task);
                lk.lock();
                continue;
            }
            seen = generation;
            const auto job = current;
            lk.unlock();
            Work(*job);
            lk.lock();
        }
    }

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<Job> current;
    u64 generation = 0;
    bool stop = false;
    std::deque<Task> async_tasks;
    std::deque<Epoch> epochs; ///< back() is open for new copies
    u64 epoch_base = 0;       ///< epoch number of epochs.front()
    std::size_t pending = 0;
    bool draining = false;
};

Pool& GetPool() {
    static Pool pool;
    return pool;
}

struct Batch {
    std::vector<Item> items;
    unsigned long long bytes = 0;
};
thread_local Batch batch;

} // namespace

bool Enabled() {
    return GetPool().Enabled() && !BbToggle::Disabled(BbToggle::ParallelCopies);
}

void Async(std::function<void()> task) {
    if (!Enabled()) {
        // The runtime toggle may change while a synchronous worker job is running.
        // Inline async fallback must not retain that job's fault mailbox either.
        CopyScope scope{nullptr};
        task();
        return;
    }
    GetPool().Async(std::move(task));
}

void FlushBatch() {
    if (batch.items.empty()) {
        return;
    }
    auto items = std::make_shared<std::vector<Item>>(std::move(batch.items));
    batch.items = {};
    batch.items.reserve(256);
    batch.bytes = 0;
    Async([items] {
        for (const auto& item : *items) {
            item.run(item);
        }
    });
}

void QueueCopy(const Item& item) {
    if (!Enabled()) {
        item.run(item);
        return;
    }
    // Smaller batches (32 items / 64 KiB, done before the next fence) measured slower.
    batch.items.push_back(item);
    batch.bytes += item.size;
    if (batch.bytes >= 512 * 1024 || batch.items.size() >= 256) {
        FlushBatch();
    }
}

void AfterCopies(std::function<void()> callback) {
    FlushBatch();
    GetPool().AfterCopies(std::move(callback));
}

void WaitAsync() {
    // The caller's own batch runs here: handing it over only to wait for it costs a wakeup.
    if (!batch.items.empty()) {
        for (const auto& item : batch.items) {
            item.run(item);
        }
        batch.items.clear();
        batch.bytes = 0;
    }
    GetPool().WaitAll();
}

void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task,
                 bool relay_faults_to_caller) {
    if (count <= 1 || in_copy_thread || !Enabled()) {
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
        return;
    }
    GetPool().Run(count, task, relay_faults_to_caller);
}

bool RelayFault(void* context, void* address, bool (*handler)(void*, void*), bool& handled) {
    auto* mailbox = fault_mailbox;
    if (!mailbox || std::this_thread::get_id() == mailbox->owner) return false;
    BbStats::copy_fault_relays.fetch_add(1, std::memory_order_relaxed);
    FaultRequest request{context, address, handler};
    request.next = mailbox->pending.load(std::memory_order_relaxed);
    while (!mailbox->pending.compare_exchange_weak(request.next, &request,
               std::memory_order_release, std::memory_order_relaxed)) {}
    while (!request.complete.load(std::memory_order_acquire)) std::this_thread::yield();
    handled = request.handled;
    if (!handled) BbStats::copy_fault_declines.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace BbCopy
