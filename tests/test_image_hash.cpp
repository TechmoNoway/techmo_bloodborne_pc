// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_image_hash.h"
#include "bbport_copy.h"
#include <windows.h>
#include <xxhash.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL image hash line %d: %s\n",__LINE__,#x); std::_Exit(1); } } while(false)
int main() {
    constexpr size_t Size=8u<<20;
    auto* data=static_cast<unsigned char*>(VirtualAlloc(nullptr,Size,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    CHECK(data);
    for(size_t i=0;i<Size;++i) data[i]=static_cast<unsigned char>(i*13+(i>>12));
    const std::array<BbImageHash::Range,4> ranges{{{0,4u<<20},{4u<<20,2u<<20},{6u<<20,1u<<20},{7u<<20,1u<<20}}};
    std::array<std::uint64_t,4> expected{},actual{};
    for(size_t i=0;i<ranges.size();++i) expected[i]=XXH3_64bits(data+ranges[i].offset,ranges[i].size);
    CHECK(BbImageHash::SnapshotParallel(data,Size,ranges,actual)==BbCopy::Enabled());
    if(BbCopy::Enabled()) CHECK(actual==expected);
    const auto saved=actual;
    auto invalid=ranges; invalid[2]={Size-1,2};
    CHECK(!BbImageHash::SnapshotParallel(data,Size,invalid,actual) && actual==saved);
    CHECK(!BbImageHash::SnapshotParallel(data,1024,ranges,actual) && actual==saved);
    DWORD old;
    CHECK(VirtualProtect(data,Size,PAGE_NOACCESS,&old));
    CHECK(!BbImageHash::SnapshotParallel(data,Size,ranges,actual) && actual==saved);
    CHECK(VirtualProtect(data,Size,PAGE_READWRITE,&old));
    data[5u<<20]^=0x37;
    for(size_t i=0;i<ranges.size();++i) {
        const auto hash=BbImageHash::Serial(data+ranges[i].offset,ranges[i].size);
        CHECK((hash==expected[i])==(i!=1));
        expected[i]=hash;
    }
    if(BbCopy::Enabled()) {
        CHECK(BbImageHash::SnapshotParallel(data,Size,ranges,actual) && actual==expected);
        const auto time=[&](bool parallel) {
            const auto begin=std::chrono::steady_clock::now();
            for(unsigned pass=0;pass<12;++pass) {
                if(parallel) CHECK(BbImageHash::SnapshotParallel(data,Size,ranges,actual));
                else for(size_t i=0;i<ranges.size();++i) actual[i]=BbImageHash::Serial(data+ranges[i].offset,ranges[i].size);
                CHECK(actual==expected);
            }
            return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()/12;
        };
        std::printf("Hash experiment, 8 MiB: serial %.3f ms, owned snapshot+parallel %.3f ms (not gameplay FPS)\n",time(false),time(true));
    }
    CHECK(VirtualFree(data,0,MEM_RELEASE));
    std::puts("PASS: exact XXH3 mip hashes, changed mip, unreadable/invalid ranges and snapshot ownership");
}
