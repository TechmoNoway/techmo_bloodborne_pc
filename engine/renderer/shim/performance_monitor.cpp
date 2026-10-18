// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include "performance_monitor.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace PerformanceMonitor {
namespace {
using Clock=std::chrono::steady_clock;
bool Valid(DWORD status) { return status==PDH_CSTATUS_VALID_DATA || status==PDH_CSTATUS_NEW_DATA; }
std::int64_t Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

struct Query {
    PDH_HQUERY query{};
    PDH_HCOUNTER cpu{},gpu{};
    std::vector<std::max_align_t> buffer;
    ~Query() { Close(); }
    void Close() { if(query)PdhCloseQuery(query); query=nullptr;cpu=nullptr;gpu=nullptr; }
    double Cpu() {
        if(!cpu)return -1;
        PDH_FMT_COUNTERVALUE value{};
        if(PdhGetFormattedCounterValue(cpu,PDH_FMT_DOUBLE,nullptr,&value)!=ERROR_SUCCESS ||
            !Valid(value.CStatus) || !std::isfinite(value.doubleValue) || value.doubleValue<0) return -1;
        return std::clamp(value.doubleValue,0.,100.);
    }
    double Gpu(Adapter adapter) {
        if(!gpu)return -1;
        // Instances can appear/disappear between the size and data calls. Retry from a new
        // zero-size query, with bounded storage; no allocations occur on the drawing thread.
        for(int attempt=0;attempt<3;++attempt) {
            DWORD bytes=0,count=0;
            if(PdhGetFormattedCounterArrayW(gpu,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,nullptr)!=static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
                !bytes || bytes>16*1024*1024) return -1;
            buffer.resize((bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
            auto* items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            const auto status=PdhGetFormattedCounterArrayW(gpu,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,&bytes,&count,items);
            if(status==static_cast<PDH_STATUS>(PDH_MORE_DATA))continue;
            if(status!=ERROR_SUCCESS)return -1;
            GpuTotal total(adapter);
            for(DWORD i=0;i<count;++i)
                if(items[i].szName && Valid(items[i].FmtValue.CStatus))
                    total.Add(items[i].szName,items[i].FmtValue.doubleValue);
            return total.Value();
        }
        return -1;
    }
};
}

struct Monitor::Impl {
    const Adapter adapter;
    std::atomic<std::uint64_t> request{0}, published{0};
    std::atomic<double> cpu{-1},gpu{-1};
    std::atomic<std::int64_t> sampled_at{0};
    std::mutex wait_mutex;
    std::condition_variable cv;
    std::jthread worker; // last: joined before the state it accesses is destroyed
    explicit Impl(Adapter id):adapter(id){}
    ~Impl() {
        if(worker.joinable()) {
            { std::lock_guard lock(wait_mutex);worker.request_stop(); }
            cv.notify_all();worker.join();
        }
    }

    void Publish(std::uint64_t generation,Snapshot sample) {
        if(request.load(std::memory_order_acquire)!=generation)return;
        cpu.store(sample.cpu,std::memory_order_relaxed);gpu.store(sample.gpu,std::memory_order_relaxed);
        sampled_at.store(Now(),std::memory_order_relaxed);
        published.store(generation,std::memory_order_release);
    }
    void Run(std::stop_token stop) {
        SetThreadDescription(GetCurrentThread(),L"Techmo CPU/GPU overlay");
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
        Query query;std::uint64_t active=0;
        Clock::time_point next{},retry{};
        bool cpu_reported=false,gpu_reported=false,query_reported=false;
        while(!stop.stop_requested()) {
            const auto wanted=request.load(std::memory_order_acquire);
            const auto mask=wanted&3;
            if(!mask) {
                query.Close();active=wanted;
                std::unique_lock lock(wait_mutex);
                cv.wait(lock,[&]{return stop.stop_requested() || (request.load()&3)!=0;});
                continue;
            }
            if(wanted!=active) {
                query.Close();active=wanted;retry=Clock::time_point{};next=Clock::now();
                Publish(active,{});
            }
            const auto now=Clock::now();
            if(now>=retry) {
                if(!query.query) {
                    const auto status=PdhOpenQueryW(nullptr,0,&query.query);
                    if(status!=ERROR_SUCCESS) {
                        query.query=nullptr;
                        if(!query_reported){std::printf("Overlay metrics: Windows counters unavailable (0x%08lx); showing N/A\n",static_cast<unsigned long>(status));query_reported=true;}
                    }
                }
                if(query.query && (mask&1) && !query.cpu) {
                    const auto status=PdhAddEnglishCounterW(query.query,L"\\Processor Information(_Total)\\% Processor Time",0,&query.cpu);
                    if(status!=ERROR_SUCCESS) {
                        query.cpu=nullptr;
                        if(!cpu_reported){std::printf("Overlay metrics: CPU counter unavailable (0x%08lx); showing N/A\n",static_cast<unsigned long>(status));cpu_reported=true;}
                    }
                }
                if(query.query && (mask&2) && !query.gpu && adapter.valid) {
                    const auto status=PdhAddEnglishCounterW(query.query,L"\\GPU Engine(*)\\Utilization Percentage",0,&query.gpu);
                    if(status!=ERROR_SUCCESS) {
                        query.gpu=nullptr;
                        if(!gpu_reported){std::printf("Overlay metrics: GPU counter unavailable (0x%08lx); showing N/A\n",static_cast<unsigned long>(status));gpu_reported=true;}
                    }
                }
                if((mask&2) && !adapter.valid && !gpu_reported) {
                    std::printf("Overlay metrics: Vulkan adapter has no valid Windows LUID; GPU shows N/A\n");gpu_reported=true;
                }
                retry=now+std::chrono::seconds(10);
            }
            if(now>=next) {
                Snapshot sample;
                if(query.query && PdhCollectQueryData(query.query)==ERROR_SUCCESS) {
                    if(mask&1)sample.cpu=query.Cpu();
                    if(mask&2)sample.gpu=query.Gpu(adapter);
                }
                Publish(active,sample);
                next=Clock::now()+std::chrono::milliseconds(500);
            }
            std::unique_lock lock(wait_mutex);
            cv.wait_until(lock,next,[&]{return stop.stop_requested() || request.load()!=active;});
        }
    }
};

Monitor::Monitor(Adapter adapter):impl(std::make_unique<Impl>(adapter)){}
Monitor::~Monitor()=default;
void Monitor::Request(bool cpu,bool gpu) {
    const std::uint64_t mask=(cpu?1:0)|(gpu?2:0);
    if((impl->request.load(std::memory_order_acquire)&3)==mask)return;
    {
        std::lock_guard lock(impl->wait_mutex);
        const auto old=impl->request.load(std::memory_order_acquire);
        if((old&3)==mask)return;
        impl->request.store(((old&~std::uint64_t(3))+4)|mask,std::memory_order_release);
    }
    if(mask && !impl->worker.joinable()) {
        try { impl->worker=std::jthread([state=impl.get()](std::stop_token stop){
            try {state->Run(stop);}catch(...){std::printf("Overlay metrics: sampling failed; showing N/A\n");}
        }); }
        catch(...){std::printf("Overlay metrics: sampler could not start; showing N/A\n");}
    }
    impl->cv.notify_all();
}
Snapshot Monitor::Read() const {
    const auto wanted=impl->request.load(std::memory_order_acquire);
    if(!(wanted&3) || impl->published.load(std::memory_order_acquire)!=wanted ||
        Now()-impl->sampled_at.load(std::memory_order_relaxed)>2500) return {};
    return {wanted&1?impl->cpu.load(std::memory_order_relaxed):-1,
            wanted&2?impl->gpu.load(std::memory_order_relaxed):-1};
}
} // namespace PerformanceMonitor
