// SPDX-License-Identifier: GPL-2.0-or-later
// Execute real PM4 coroutine paths without a guest or Vulkan device. Track only
// this thread's allocations, after service warmup, to detect leaked child frames.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include <crtdbg.h>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"

namespace {
thread_local bool watching;
thread_local std::array<void*, 4096> live;
thread_local size_t outstanding, allocations;
}
void* operator new(size_t size) {
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc{};
    if (watching) {
        assert(outstanding < live.size());
        live[outstanding++] = p; ++allocations;
    }
    return p;
}
void operator delete(void* p) noexcept {
    if (p && watching) {
        for (size_t i = 0; i < outstanding; ++i) {
            if (live[i] == p) { live[i] = live[--outstanding]; break; }
        }
    }
    std::free(p);
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }

using namespace AmdGpu;
using Stream = std::vector<u32>;
u32 Header(PM4ItOpcode op, u32 payload) {
    PM4Type3Header header{op, payload - 1};
    u32 word; std::memcpy(&word, &header, 4); return word;
}
void Append(Stream& dst, PM4ItOpcode op, std::initializer_list<u32> data) {
    dst.push_back(Header(op, data.size())); dst.insert(dst.end(), data);
}
void Indirect(Stream& dst, PM4ItOpcode op, const Stream& child) {
    const auto address = reinterpret_cast<uintptr_t>(child.data());
    assert((address >> 48) == 0);
    Append(dst, op, {u32(address), u32(address >> 32), u32(child.size())});
}
namespace AmdGpu {
struct LiverpoolTaskTest {
    static void Run(Liverpool& gpu, const Stream& stream, int kind) {
        auto task = kind == 0 ? gpu.ProcessCeUpdate(stream) :
                    kind == 1 ? gpu.ProcessGraphics(stream, {}) :
                                gpu.ProcessCompute<true>(stream, 0);
        while (!task.handle.done()) task.handle.resume();
        task.handle.destroy();
    }
    static void YieldedCe(Liverpool& gpu, const Stream& stream) {
        gpu.cblock.ce_count = 0; gpu.cblock.de_count = 2;
        auto task = gpu.ProcessCeUpdate(stream);
        task.handle.resume();
        assert(!task.handle.done());
        gpu.cblock.ce_count = 2;
        task.handle.resume(); assert(task.handle.done()); task.handle.destroy();
    }
};
}
int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _putenv_s("BB_NULL_GPU", "1");
    Liverpool gpu;
    gpu.asc_queues.insert(Liverpool::AscQueueInfo{});
    std::array<std::array<Stream, 17>, 3> chains;
    for (int kind = 0; kind < 3; ++kind) {
        Append(chains[kind][0], PM4ItOpcode::Nop, {0});
        for (size_t depth = 1; depth < chains[kind].size(); ++depth) {
            Indirect(chains[kind][depth], kind == 0 ? PM4ItOpcode::IndirectBufferConst :
                     PM4ItOpcode::IndirectBuffer, chains[kind][depth - 1]);
        }
        for (int warm = 0; warm < 5; ++warm)
            LiverpoolTaskTest::Run(gpu, chains[kind].back(), kind);
        watching = true;
        const auto before = allocations;
        for (int pass = 0; pass < 1000; ++pass) {
            LiverpoolTaskTest::Run(gpu, chains[kind].back(), kind);
            if (outstanding) {
                std::fprintf(stderr, "Leaked %zu allocations: engine %d iteration %d\n", outstanding, kind, pass);
                std::abort();
            }
        }
        assert(allocations > before);
        watching = false;
        std::printf("PASS: engine %d, 1000 submissions with 16 nested IBs, %zu allocations balanced\n", kind, allocations - before);
    }
    Stream wait, parent;
    Append(wait, PM4ItOpcode::WaitOnDeCounterDiff, {1});
    Indirect(parent, PM4ItOpcode::IndirectBufferConst, wait);
    LiverpoolTaskTest::YieldedCe(gpu, parent); // warm up
    watching = true;
    LiverpoolTaskTest::YieldedCe(gpu, parent);
    assert(outstanding == 0);
    watching = false;
    puts("PASS: yielded CE child survives suspension and is destroyed on completion");

    // All indirect variants restore INDEX_BUFFER_SIZE after a direct draw.
    for (auto op : {PM4ItOpcode::DrawIndexIndirect, PM4ItOpcode::DrawIndexIndirectMulti,
                    PM4ItOpcode::DrawIndexIndirectCountMulti}) {
        Stream direct, indirect;
        Append(direct, PM4ItOpcode::IndexBufferSize, {1234});
        Append(direct, PM4ItOpcode::DrawIndexOffset2, {16, 0, 3, 0});
        LiverpoolTaskTest::Run(gpu, direct, 1);
        assert(gpu.regs.max_index_size == 16 && gpu.regs.num_indices == 3);
        assert(gpu.index_buffer_num_indices == 1234);
        indirect.resize((op == PM4ItOpcode::DrawIndexIndirect ?
            sizeof(PM4CmdDrawIndexIndirect) : op == PM4ItOpcode::DrawIndexIndirectMulti ?
            sizeof(PM4CmdDrawIndexIndirectMulti) : sizeof(PM4CmdDrawIndexIndirectCountMulti)) / 4);
        indirect[0] = Header(op, indirect.size() - 1);
        LiverpoolTaskTest::Run(gpu, indirect, 1);
        assert(gpu.regs.max_index_size == 1234 && gpu.regs.num_indices == 3);
    }
    puts("PASS: indirect index capacity persists across direct draws and submissions, all three variants");
}
