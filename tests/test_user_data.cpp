// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from DarkIzuku dceac3f478; snapshots and direct-register bounds included.
#include "shader_recompiler/info.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL user data line %d: %s\n",__LINE__,#x); std::_Exit(1); } } while (false)
int main() {
    Shader::Info info;
    std::array<u32,4> data{};
    info.user_data=data;
    CHECK(info.UdPointerNull(0) && info.UdPointerNull(3));
    CHECK(info.UdPointerNull(std::numeric_limits<u32>::max()));
    CHECK(!info.UdPointerNull(Shader::IR::NumScalarRegs));
    CHECK(info.ReadUdReg<u32>(0,100)==0);
    data[0]=0xffff; CHECK(info.UdPointerNull(0));
    data[0]=0; data[1]=0xabcd0000; CHECK(info.UdPointerNull(0));
    std::array<u32,4> table{11,22,33,44};
    const auto pointer=reinterpret_cast<uintptr_t>(table.data());
    std::memcpy(data.data(),&pointer,sizeof(pointer));
    CHECK(!info.UdPointerNull(0) && info.ReadUdReg<u32>(0,2)==33);
    const std::array<u32,2> missing{};
    Shader::Info::ud_snapshots[0]={&info,missing.data(),2,nullptr,0,0};
    Shader::Info::num_ud_snapshots=1;
    CHECK(info.UdPointerNull(0) && info.ReadUdReg<u32>(0,0)==0);
    Shader::Info::num_ud_snapshots=0;
    CHECK(info.ReadUdReg<u32>(0,2)==33);
    CHECK(info.ReadUdReg<u32>(Shader::IR::NumScalarRegs,3)==data[3]);
    CHECK(info.ReadUdReg<u64>(Shader::IR::NumScalarRegs,3)==0);
    CHECK(info.ReadUdReg<u32>(Shader::IR::NumScalarRegs,std::numeric_limits<u32>::max())==0);
    info.tess_consts_ptr_base=Shader::IR::ScalarReg(0); info.tess_consts_dword_offset=0;
    data={}; Shader::TessellationDataConstantBuffer tess{};
    info.ReadTessConstantBuffer(tess);
    std::puts("PASS: null/incomplete/tagged descriptor tables, valid reads, direct bounds, tessellation and stage snapshots");
}
