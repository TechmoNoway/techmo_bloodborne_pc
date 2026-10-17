// SPDX-License-Identifier: GPL-2.0-or-later
// Regressions for shadPS4 #5220 and #5366, using the production CFG and SPIR-V backend.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sirit/sirit.h>
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL shader line %d: %s\n",__LINE__,#x); std::_Exit(1); } } while (false)
static void CheckExec(bool reopen) {
    using namespace Shader;
    std::array<Gcn::GcnInst,5> instructions{};
    for (auto& inst:instructions) inst.length=4;
    instructions[0].opcode=Gcn::Opcode::S_AND_SAVEEXEC_B64;
    instructions[0].category=Gcn::InstCategory::ScalarALU;
    instructions[1].opcode=reopen ? Gcn::Opcode::S_ANDN2_B64 : Gcn::Opcode::S_MOV_B64;
    instructions[1].category=Gcn::InstCategory::ScalarALU;
    instructions[1].dst[0].field=Gcn::OperandField::ExecLo;
    instructions[2].opcode=Gcn::Opcode::V_MOV_B32;
    instructions[2].category=Gcn::InstCategory::VectorALU;
    instructions[3].opcode=Gcn::Opcode::S_MOV_B64;
    instructions[3].category=Gcn::InstCategory::ScalarALU;
    instructions[3].dst[0].field=Gcn::OperandField::ExecLo;
    instructions[4].opcode=Gcn::Opcode::S_ENDPGM;
    instructions[4].category=Gcn::InstCategory::FlowControl;
    Common::ObjectPool<Gcn::Block> pool;
    Gcn::CFG cfg(pool,instructions);
    bool vector_guarded=false;
    for (const auto& block:cfg) {
        if (block.cond==IR::Condition::Execnz && block.branch_true && block.branch_false) {
            vector_guarded|=block.branch_true->begin_index==2 && block.branch_true->end_index==2;
        }
    }
    CHECK(vector_guarded==reopen);
}
static void CheckU64(const char* path) {
    using namespace Shader;
    Info info{}; info.hw_stage=HwStage::Compute; info.sw_stage=SwStage::Compute;
    RuntimeInfo runtime{}; runtime.Initialize(info.hw_stage,info.sw_stage);
    runtime.hw.cs.workgroup_size={1,1,1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    block.AppendNewInst(IR::Opcode::UndefU64,{});
    IR::IREmitter ir(block); ir.Epilogue();
    IR::Program program(info); program.blocks.push_back(&block);
    program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
    Profile profile{}; profile.supported_spirv=0x00010600; profile.support_int64=true;
    Backend::Bindings bindings{};
    const auto code=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    u32 type=0; bool undefined=false,capability=false;
    for (size_t i=5;i<code.size();) {
        const auto count=code[i]>>16; CHECK(count && count<=code.size()-i);
        const auto op=spv::Op(code[i]&0xffff);
        if (op==spv::Op::OpTypeInt && code[i+2]==64 && code[i+3]==0) type=code[i+1];
        if (op==spv::Op::OpUndef && type && code[i+1]==type) undefined=true;
        if (op==spv::Op::OpCapability && code[i+1]==u32(spv::Capability::Int64)) capability=true;
        i+=count;
    }
    CHECK(type && undefined && capability);
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(code.data()),code.size()*sizeof(u32)); CHECK(bool(output));
}
int main(int argc,char** argv) {
    const char* mode=argc>1 ? argv[1] : "all";
    if (std::strcmp(mode,"u64")) { CheckExec(true); CheckExec(false); }
    if (std::strcmp(mode,"cfg")) CheckU64(argc>2 ? argv[2] : "undef-u64.spv");
    std::puts("PASS: empty EXEC close/reopen preserves vector masking; UndefU64 emits a 64-bit OpUndef");
}
