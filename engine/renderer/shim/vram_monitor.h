// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_4.h>
#endif

namespace VideoMemory {
// valid requires a discrete renderer GPU with a Windows LUID; UMA local memory is RAM.
struct Adapter { std::uint32_t low{}, high{}, nodes{1}; bool valid{}; };
struct Sample { double used{-1}, total{-1}; }; // bytes, -1 = unavailable
inline bool Known(Sample s) { return std::isfinite(s.used) && s.used >= 0 && std::isfinite(s.total) && s.total > 0; }
inline std::string Text(Sample s) {
    if (!Known(s)) return "VRAM N/A";
    char text[96];
    constexpr double gib = 1073741824.;
    std::snprintf(text, sizeof(text), "VRAM %.1f / %.1f GiB", s.used/gib, s.total/gib);
    return text;
}
inline bool Matches(Adapter a, std::uint32_t low, std::uint32_t high) {
    return a.valid && a.nodes != 0 && a.low == low && a.high == high;
}

#ifdef _WIN32
// DXGI LOCAL CurrentUsage is this process's GPU memory, not system RAM or all apps.
// DedicatedVideoMemory is physical capacity; the changing WDDM Budget is not capacity.
class Query {
    HMODULE library{};
    IDXGIAdapter3* adapter{};
    double capacity{-1};
public:
    ~Query() { Close(); }
    void Close() {
        if (adapter) adapter->Release();
        adapter = nullptr; capacity = -1;
        if (library) FreeLibrary(library);
        library = nullptr;
    }
    bool Open(Adapter selected) {
        if (adapter) return true;
        if (!selected.valid || !selected.nodes) return false;
        Close(); library = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) return false;
        using Create = HRESULT(WINAPI*)(REFIID, void**);
        const auto create = reinterpret_cast<Create>(GetProcAddress(library, "CreateDXGIFactory1"));
        constexpr GUID factory_id{0x770aae78,0xf26f,0x4dba,{0xa8,0x29,0x25,0x3c,0x83,0xd1,0xb3,0x87}};
        constexpr GUID adapter_id{0x645967a4,0x1392,0x4310,{0xa7,0x98,0x80,0x53,0xce,0x3e,0x93,0xfd}};
        IDXGIFactory1* factory = nullptr;
        if (!create || FAILED(create(factory_id, reinterpret_cast<void**>(&factory)))) { Close(); return false; }
        for (UINT index = 0; ; ++index) {
            IDXGIAdapter1* candidate = nullptr;
            if (FAILED(factory->EnumAdapters1(index, &candidate))) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(candidate->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                desc.DedicatedVideoMemory > 0 && Matches(selected, desc.AdapterLuid.LowPart, std::uint32_t(desc.AdapterLuid.HighPart)) &&
                SUCCEEDED(candidate->QueryInterface(adapter_id, reinterpret_cast<void**>(&adapter)))) {
                capacity = double(desc.DedicatedVideoMemory);
            }
            candidate->Release();
            if (adapter) break;
        }
        factory->Release();
        if (!adapter) { Close(); return false; }
        return true;
    }
    Sample Read(Adapter selected) {
        if (!adapter) return {};
        double used = 0;
        for (UINT node = 0; node < 32; ++node) {
            if (!(selected.nodes & (std::uint32_t(1) << node))) continue;
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (FAILED(adapter->QueryVideoMemoryInfo(node, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) { Close(); return {}; }
            used += double(info.CurrentUsage);
        }
        return {used, capacity};
    }
};
#endif

// Only the worker calls DXGI. The overlay reads atomics and never waits for a driver.
class Monitor {
    const Adapter adapter;
    std::atomic<std::uint64_t> requested{0}, published{0};
    std::atomic<double> used{-1}, total{-1};
    std::atomic<std::int64_t> sampled{0};
    std::mutex mutex;
    std::condition_variable wake;
    std::jthread worker;
    static std::int64_t Now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void Run(std::stop_token stop) {
#ifdef _WIN32
        SetThreadDescription(GetCurrentThread(), L"Techmo VRAM overlay");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Query query;
        auto retry = std::chrono::steady_clock::time_point{};
        std::uint64_t previous = 0;
#endif
        while (!stop.stop_requested()) {
            const auto generation = requested.load(std::memory_order_acquire);
#ifdef _WIN32
            if (generation != previous) { query.Close(); retry = {}; previous = generation; }
#endif
            if (!(generation & 1)) {
#ifdef _WIN32
                query.Close();
                retry = std::chrono::steady_clock::time_point{};
#endif
                std::unique_lock lock(mutex);
                wake.wait(lock, [&]{ return stop.stop_requested() || (requested.load() & 1); });
                continue;
            }
            Sample value;
#ifdef _WIN32
            const auto now = std::chrono::steady_clock::now();
            if (now >= retry) {
                if (query.Open(adapter)) value = query.Read(adapter);
                retry = now + (Known(value) ? std::chrono::milliseconds(500) : std::chrono::milliseconds(10000));
            }
#endif
            if (requested.load(std::memory_order_acquire) == generation) {
                used.store(value.used, std::memory_order_relaxed);
                total.store(value.total, std::memory_order_relaxed);
                sampled.store(Now(), std::memory_order_relaxed);
                published.store(generation, std::memory_order_release);
            }
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(500), [&]{ return stop.stop_requested() || requested.load() != generation; });
        }
    }
public:
    explicit Monitor(Adapter selected) : adapter(selected) {}
    ~Monitor() {
        if (worker.joinable()) { { std::lock_guard lock(mutex); worker.request_stop(); } wake.notify_all(); worker.join(); }
    }
    void Request(bool enabled) {
        if (bool(requested.load(std::memory_order_acquire) & 1) == enabled) return;
        {
            std::lock_guard lock(mutex);
            const auto state = requested.load(std::memory_order_acquire);
            if (bool(state & 1) == enabled) return;
            requested.store(((state & ~std::uint64_t(1)) + 2) | std::uint64_t(enabled), std::memory_order_release);
            if (enabled && !worker.joinable()) {
                try { worker = std::jthread([this](std::stop_token stop){ try { Run(stop); } catch (...) { std::puts("Overlay VRAM: sampling unavailable"); } }); }
                catch (...) { std::puts("Overlay VRAM: worker unavailable"); }
            }
        }
        wake.notify_all();
    }
    Sample Read() const {
        const auto generation = requested.load(std::memory_order_acquire);
        if (!(generation & 1) || published.load(std::memory_order_acquire) != generation ||
            Now() - sampled.load(std::memory_order_relaxed) > 2500) return {};
        return {used.load(std::memory_order_relaxed), total.load(std::memory_order_relaxed)};
    }
};
} // namespace VideoMemory
