// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <list>
#include <random>
#include <unordered_map>
#include <vector>
#include "common/lru_cache.h"
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL LRU line %d: %s\n",__LINE__,#x);std::_Exit(1); } } while(false)
int main() {
    Common::LeastRecentlyUsedCache<u32,u64> quota;
    for(u32 i=0;i<64;++i) quota.Insert(i,i);
    unsigned visits=0;quota.ForEachItemBelow(64,[&](u32) { return ++visits==3; });CHECK(visits==3);
    Common::LeastRecentlyUsedCache<u32,u64> cache;
    std::unordered_map<u32,size_t> handles;
    std::list<std::pair<u32,u64>> reference;
    std::mt19937 random(0x5219);u32 sequence=0;u64 tick=0;
    const auto verify=[&](u64 cutoff) {
        std::vector<u32> expected,actual;
        for(auto [value,time]:reference) if(time<=cutoff) expected.push_back(value);
        cache.ForEachItemBelow(cutoff,[&](u32 value) { actual.push_back(value); });CHECK(actual==expected);
    };
    for(u32 step=0;step<50000;++step) {
        ++tick;
        const auto operation=random()%4;
        if(reference.empty() || (operation==0 && reference.size()<4096)) {
            handles.emplace(sequence,cache.Insert(sequence,tick));reference.emplace_back(sequence++,tick);
        } else if(operation==1) {
            auto it=reference.begin();std::advance(it,random()%reference.size());
            cache.Touch(handles.at(it->first),tick);it->second=tick;reference.splice(reference.end(),reference,it);
        } else if(operation==2) {
            auto it=reference.begin();std::advance(it,random()%reference.size());
            cache.Free(handles.at(it->first));handles.erase(it->first);reference.erase(it);
        } else {
            unsigned remaining=3;std::vector<u32> removed;
            cache.ForEachItemBelow(tick,[&](u32 value) {
                removed.push_back(value);cache.Free(handles.at(value));return --remaining==0;
            });
            for(u32 value:removed) { CHECK(reference.front().first==value);reference.pop_front();handles.erase(value); }
        }
        if(step%101==0) { verify(tick);verify(tick>50?tick-50:0); }
    }
    // Growth moves the backing vector; public IDs and ordering must stay valid.
    for(u32 i=0;i<10000;++i) { handles.emplace(sequence,cache.Insert(sequence,++tick));reference.emplace_back(sequence++,tick); }
    verify(tick);
    cache.ForEachItemBelow(tick,[&](u32 value) { cache.Free(handles.at(value)); });
    visits=0;cache.ForEachItemBelow(tick,[&](u32) { ++visits; });CHECK(visits==0);
    const auto id=cache.Insert(7,++tick);cache.Touch(id,tick);cache.Touch(id,tick-1);
    visits=0;cache.ForEachItemBelow(tick,[&](u32 value) { CHECK(value==7);++visits; });CHECK(visits==1);
    puts("PASS: bounded boolean callbacks, 50k reference-model operations, growth/reuse, deletion during traversal and monotonic touches");
}
