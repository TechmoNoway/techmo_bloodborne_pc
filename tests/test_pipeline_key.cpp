// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL pipeline key line %d: %s\n",__LINE__,#x); std::exit(1); } } while(false)
int main(int argc,char**) {
    using Vulkan::GraphicsPipelineKey;
    GraphicsPipelineKey key;
    key.prim_type=AmdGpu::PrimitiveType::TriangleList;
    key.stage_hashes[3]=0x123456;
    key.color_buffers[0].data_format=AmdGpu::DataFormat::Format8_8_8_8;
    key.num_color_attachments=1;
    const auto hash=std::hash<GraphicsPipelineKey>{}(key);
    CHECK(hash==XXH3_64bits(&key,sizeof(key))); // Canonical zero-padded v6 hashes survive.
    std::function<GraphicsPipelineKey()> capture=[key] { return key; };
    auto copy=capture();
    CHECK(key==copy && hash==std::hash<GraphicsPipelineKey>{}(copy));
    std::unordered_map<GraphicsPipelineKey,int> cache;
    cache.emplace(key,123);CHECK(cache.at(copy)==123);
    unsigned padding_bits=0;
    for(size_t byte=0;byte<sizeof(key);++byte) for(unsigned bit=0;bit<8;++bit) {
        GraphicsPipelineKey altered;
        std::memcpy(&altered,&key,sizeof(key));
        reinterpret_cast<unsigned char*>(&altered)[byte]^=1u<<bit;
        if(altered==key) {
            ++padding_bits;CHECK(std::hash<GraphicsPipelineKey>{}(altered)==hash);
            CHECK(cache.at(altered)==123);
        }
    }
    CHECK(padding_bits>0);
    auto changed=key;changed.depth_clip_enable^=1;
    CHECK(!(changed==key) && std::hash<GraphicsPipelineKey>{}(changed)!=hash);
    const auto serialized=key.CanonicalBytes();GraphicsPipelineKey loaded;
    std::memcpy(&loaded,serialized.data(),serialized.size());
    CHECK(loaded==key && std::hash<GraphicsPipelineKey>{}(loaded)==hash);
    std::printf("PASS: %u padding bits ignored; semantic fields, lambda capture, map lookup and v6 hash/layout roundtrip stay consistent\n",padding_bits);
    if(argc>1) {
        constexpr unsigned N=500000;
        size_t checksum=0;
        const auto start=std::chrono::steady_clock::now();
        for(unsigned i=0;i<N;++i) { key.stage_hashes[0]=i;checksum^=std::hash<GraphicsPipelineKey>{}(key); }
        const auto end=std::chrono::steady_clock::now();
        const double ns=std::chrono::duration<double,std::nano>(end-start).count()/N;
        std::printf("CPU microbenchmark: %.1f ns/canonical key hash, checksum=%zx (not gameplay)\n",ns,checksum);
    }
}
