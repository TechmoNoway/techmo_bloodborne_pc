// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace PerformanceMonitor {
struct Adapter {
    std::uint32_t low{}, high{}, node_mask{1};
    bool valid{};
};
struct Snapshot { double cpu{-1}, gpu{-1}; };
struct Engine {
    std::uint32_t low{}, high{}, physical{}, index{};
};

// PDH instance names identify adapter LUIDs, not stable GPU 0/1 indices.
inline std::optional<Engine> ParseEngine(std::wstring_view name) {
    auto number = [&](std::wstring_view prefix, unsigned base, std::uint32_t& out) {
        if (!name.starts_with(prefix)) return false;
        name.remove_prefix(prefix.size());
        std::uint64_t value=0; std::size_t count=0;
        for (wchar_t c : name) {
            unsigned digit = c>=L'0' && c<=L'9' ? unsigned(c-L'0') :
                c>=L'a' && c<=L'f' ? unsigned(c-L'a'+10) :
                c>=L'A' && c<=L'F' ? unsigned(c-L'A'+10) : 99;
            if (digit>=base) break;
            value=value*base+digit;
            if (value>UINT32_MAX) return false;
            ++count;
        }
        if (!count) return false;
        name.remove_prefix(count);out=std::uint32_t(value);return true;
    };
    Engine engine; std::uint32_t pid;
    if (!number(L"pid_",10,pid) || !number(L"_luid_0x",16,engine.high) ||
        !number(L"_0x",16,engine.low) || !number(L"_phys_",10,engine.physical) ||
        !number(L"_eng_",10,engine.index) || !name.starts_with(L"_engtype_") || name.size()<=9)
        return {};
    return engine;
}

class GpuTotal {
    Adapter adapter;
    std::map<std::uint64_t,double> engines;
public:
    explicit GpuTotal(Adapter id) : adapter(id) {}
    void Add(std::wstring_view name, double value) {
        if (!adapter.valid || !std::isfinite(value) || value<0) return;
        const auto engine=ParseEngine(name);
        if (!engine || engine->low!=adapter.low || engine->high!=adapter.high ||
            engine->physical>=32 || !(adapter.node_mask & (1u<<engine->physical))) return;
        const auto key=(std::uint64_t(engine->physical)<<32)|engine->index;
        // Sum processes sharing the SAME engine, then take the busiest engine. Independent
        // 3D/copy/compute engines run concurrently and must not be summed into >100% GPU use.
        engines[key]+=value;
    }
    double Value() const {
        double value=-1;
        for (const auto& [key,load]:engines) value=std::max(value,std::clamp(load,0.,100.));
        return value;
    }
};

inline std::string OverlayText(bool fps_on, bool cpu_on, bool gpu_on, double fps,
                               double ms, const char* upscaler, Snapshot sample) {
    std::string result; char text[160];
    if (fps_on) {
        std::snprintf(text,sizeof(text),"%.0f FPS  %.1f ms  %s",fps,ms,upscaler);
        result=text;
    }
    const auto append=[&](const char* name,double value) {
        if (!result.empty()) result+="  |  ";
        if (std::isfinite(value) && value>=0)
            std::snprintf(text,sizeof(text),"%s %.0f%%",name,std::clamp(value,0.,100.));
        else std::snprintf(text,sizeof(text),"%s N/A",name);
        result+=text;
    };
    if(cpu_on)append("CPU",sample.cpu);
    if(gpu_on)append("GPU",sample.gpu);
    return result;
}

// Request/Read are called on the overlay thread. PDH enumeration/collection only runs on
// the lazy worker. Neither drawing nor Request(false,false) waits for a Windows counter.
class Monitor {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit Monitor(Adapter adapter);
    ~Monitor();
    void Request(bool cpu, bool gpu);
    Snapshot Read() const;
};
} // namespace PerformanceMonitor
