// SPDX-License-Identifier: GPL-2.0-or-later
// Production copy pool + real Windows protected-page faults; no game or Vulkan device.
#include <windows.h>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <crtdbg.h>
#include "bbport_copy.h"
#include "bbport_toggles.h"

extern "C" {
std::uint64_t runtime_disabled_optimizations = 0;
std::uint64_t runtime_experiment_bits = 0;
}
namespace BbThreads { unsigned Available() { return 16; } }
namespace Common { void SetCurrentThreadName(const char*) {} }

namespace {
constexpr unsigned Pages = 64;
unsigned char* pages;
DWORD owner;
std::atomic<unsigned> handled_count{0}, worker_count{0};

bool Resolve(void* context, void* address) {
    assert(GetCurrentThreadId() == owner);
    auto* exception = static_cast<EXCEPTION_POINTERS*>(context);
    assert(exception->ContextRecord->Rip != 0);
    const auto page = (static_cast<unsigned char*>(address) - pages) / 4096;
    assert(page >= 0 && page < Pages);
    DWORD previous;
    auto* base = pages + page * 4096;
    assert(VirtualProtect(base, 4096, PAGE_READWRITE, &previous));
    *reinterpret_cast<unsigned*>(base) = 0xa500u + unsigned(page);
    if (exception->ExceptionRecord->ExceptionInformation[0] == 0)
        assert(VirtualProtect(base, 4096, PAGE_READONLY, &previous));
    ++handled_count;
    return true;
}

LONG CALLBACK Fault(EXCEPTION_POINTERS* exception) {
    auto* record = exception->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    auto* address = reinterpret_cast<void*>(record->ExceptionInformation[1]);
    if (reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(pages) >= Pages * 4096)
        return EXCEPTION_CONTINUE_SEARCH;
    bool handled;
    // Same relay boundary as PageManager, before consulting the GPU/cache state.
    if (BbCopy::RelayFault(exception, address, &Resolve, handled)) ++worker_count;
    else handled = Resolve(exception, address);
    return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

bool Decline(void* context, void*) {
    assert(GetCurrentThreadId() == owner);
    ++*static_cast<unsigned*>(context);
    return false;
}
struct OwnerCheck { DWORD thread = GetCurrentThreadId(); unsigned calls = 0; };
bool CheckOwner(void* context, void*) {
    auto& check = *static_cast<OwnerCheck*>(context);
    assert(GetCurrentThreadId() == check.thread);
    ++check.calls;
    return true;
}
bool NeverHandle(void*, void*) { return false; }
} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    const bool serial = argc == 2 && !std::strcmp(argv[1], "--serial");
    const bool disabled = argc == 2 && !std::strcmp(argv[1], "--disabled");
    if (serial) _putenv_s("BB_COPY_THREADS", "0");
    if (disabled) runtime_disabled_optimizations = BbToggle::ParallelCopies;
    owner = GetCurrentThreadId();
    std::jthread watchdog{[](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!stop.stop_requested()) {
            std::fputs("FAIL: copy fault relay timed out\n", stderr);
            std::_Exit(124);
        }
    }};
    pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, Pages * 4096,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
    assert(pages);
    auto* handler = AddVectoredExceptionHandler(1, &Fault);
    assert(handler);
    for (unsigned round = 0; round < 8; ++round) {
        DWORD previous;
        assert(VirtualProtect(pages, Pages * 4096, PAGE_NOACCESS, &previous));
        std::array<unsigned, Pages> results{};
        BbCopy::ParallelFor(Pages, [&](size_t index) {
            // Give workers a chance to reach a protected page while the caller waits.
            if (index == 0 && !serial && !disabled) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
                while (std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            }
            auto* value = reinterpret_cast<volatile unsigned*>(pages + index * 4096);
            if (round & 1) *value = unsigned(index) + 0x5a00;
            results[index] = *value;
            // Nested loops retain the outer synchronous owner; no second pool job.
            BbCopy::ParallelFor(2, [&](size_t) { assert(*value == results[index]); }, true);
        }, true);
        for (unsigned i = 0; i < Pages; ++i)
            assert(results[i] == ((round & 1) ? i + 0x5a00u : i + 0xa500u));
    }
    assert(handled_count == Pages * 8);
    if (!serial && !disabled) assert(worker_count > 0);
    unsigned declined = 0;
    BbCopy::ParallelFor(Pages, [&](size_t) {
        bool handled;
        if (!BbCopy::RelayFault(&declined, nullptr, &Decline, handled)) handled = Decline(&declined, nullptr);
        assert(!handled);
    }, true);
    assert(declined == Pages);
    std::atomic<unsigned> async_done{0};
    BbCopy::ParallelFor(Pages, [&](size_t) {
        BbCopy::Async([&] {
            bool handled;
            assert(!BbCopy::RelayFault(nullptr, nullptr, &Decline, handled));
            ++async_done;
        });
    }, true);
    BbCopy::WaitAsync();
    assert(async_done == Pages);
    if (!serial && !disabled) {
        std::atomic<bool> checked{false};
        BbCopy::ParallelFor(Pages, [&](size_t index) {
            if (GetCurrentThreadId() == owner && index == 0) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                while (!checked && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            }
            if (GetCurrentThreadId() != owner && !checked.exchange(true)) {
                __atomic_store_n(&runtime_disabled_optimizations, BbToggle::ParallelCopies, __ATOMIC_RELAXED);
                BbCopy::Async([] {
                    bool handled;
                    assert(!BbCopy::RelayFault(nullptr, nullptr, &NeverHandle, handled));
                });
                __atomic_store_n(&runtime_disabled_optimizations, 0, __ATOMIC_RELAXED);
            }
        }, true);
        assert(checked);
    }
    BbCopy::ParallelFor(Pages, [&](size_t) {
        bool handled;
        assert(!BbCopy::RelayFault(nullptr, nullptr, &Decline, handled));
    }); // Host-only jobs do not acquire a renderer context.
    // Two independent synchronous callers may replace the pool's advertised job.
    // Neither caller can service the other caller's requests or strand its workers.
    auto concurrent = [] {
        for (unsigned round = 0; round < 4; ++round) {
            OwnerCheck check;
            BbCopy::ParallelFor(256, [&](size_t) {
                bool handled;
                if (!BbCopy::RelayFault(&check, nullptr, &CheckOwner, handled))
                    handled = CheckOwner(&check, nullptr);
                assert(handled);
            }, true);
            assert(check.calls == 256);
        }
    };
    std::thread first{concurrent}, second{concurrent};
    first.join(); second.join();
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(pages, 0, MEM_RELEASE);
    watchdog.request_stop();
    std::printf("PASS: 512 protected-page read/write faults, %u worker relays, nested/async isolation (%s)\n",
        worker_count.load(), serial ? "serial" : disabled ? "disabled" : "parallel");
}
