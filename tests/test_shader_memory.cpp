// SPDX-License-Identifier: GPL-2.0-or-later
// Production IR passes and SPIR-V emission, without guest data or player caches.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <unordered_map>
#include <sirit/sirit.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL shader-memory line %d: %s\n",__LINE__,#x); std::_Exit(1); } } while(false)

using namespace Shader;
static unsigned Count(const IR::Block& block, IR::Opcode op) {
    unsigned count=0;
    for (const auto& inst:block.Instructions()) count+=inst.GetOpcode()==op;
    return count;
}
static void Save(const std::filesystem::path& path,const std::vector<u32>& code) {
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(code.data()),code.size()*sizeof(u32));CHECK(bool(out));
}
static void CheckFp64(const std::filesystem::path& dir) {
    unsigned count=0;
    for (u32 value=0;value<std::size(IR::Detail::META_TABLE);++value) {
        const auto op=static_cast<IR::Opcode>(value);
        const auto& meta=IR::Detail::META_TABLE[value];
        const bool fp64=meta.type==IR::Type::F64 || std::ranges::find(meta.arg_types,IR::Type::F64)!=meta.arg_types.end();
        if (!fp64) continue;
        Info info{};info.hw_stage=HwStage::Compute;info.sw_stage=SwStage::Compute;
        Common::ObjectPool<IR::Inst> pool;IR::Block block(pool);IR::IREmitter ir(block);
        std::array<IR::Value,3> args;
        CHECK(IR::NumArgsOf(op)<=args.size());
        for(size_t i=0;i<IR::NumArgsOf(op);++i) {
            switch(IR::ArgTypeOf(op,i)) {
            case IR::Type::F64:args[i]=ir.Imm64(1.0);break;
            case IR::Type::F32:args[i]=ir.Imm32(1.0f);break;
            case IR::Type::U32:args[i]=ir.Imm32(1u);break;
            case IR::Type::U32x2:args[i]=ir.CompositeConstruct(ir.Imm32(1u),ir.Imm32(0u));break;
            default:CHECK(false);
            }
        }
        switch(IR::NumArgsOf(op)) {
        case 0:block.AppendNewInst(op,{});break;
        case 1:block.AppendNewInst(op,{args[0]});break;
        case 2:block.AppendNewInst(op,{args[0],args[1]});break;
        case 3:block.AppendNewInst(op,{args[0],args[1],args[2]});break;
        }
        IR::Program program(info);program.post_order_blocks.push_back(&block);
        Optimization::CollectShaderInfoPass(program,{});
        if(!info.uses_fp64) std::fprintf(stderr,"missing FP64 opcode: %.*s\n",int(meta.name.size()),meta.name.data());
        CHECK(info.uses_fp64);++count;
    }
    CHECK(count>=38);
    Info info{};info.hw_stage=HwStage::Compute;info.sw_stage=SwStage::Compute;
    RuntimeInfo runtime{};runtime.Initialize(info.hw_stage,info.sw_stage);runtime.hw.cs.workgroup_size={1,1,1};
    Common::ObjectPool<IR::Inst> pool;IR::Block block(pool);IR::IREmitter ir(block);
    // No PackDouble/UnpackDouble operation: old collection missed this shader.
    const auto value=ir.FPAdd(ir.Imm64(1.25),ir.Imm64(2.5));(void)value;ir.Epilogue();
    IR::Program program(info);program.blocks.push_back(&block);program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};profile.supported_spirv=0x00010600;
    Optimization::CollectShaderInfoPass(program,profile);CHECK(info.uses_fp64);
    Backend::Bindings bindings{};const auto code=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    bool capability=false,add=false;
    for(size_t i=5;i<code.size();) {
        const u32 n=code[i]>>16;CHECK(n && n<=code.size()-i);const auto op=spv::Op(code[i]&0xffff);
        capability|=op==spv::Op::OpCapability && code[i+1]==u32(spv::Capability::Float64);
        add|=op==spv::Op::OpFAdd;i+=n;
    }
    CHECK(capability && add);Save(dir/"fp64-add.spv",code);
    std::printf("PASS: %u FP64-typed opcodes detected; arithmetic-only shader emits Float64 capability\n",count);
}
static void CheckBarriers(u32 threads,bool needs,bool memory,const std::filesystem::path& dir) {
    Info info{};info.hw_stage=HwStage::Compute;info.sw_stage=SwStage::Compute;
    RuntimeInfo runtime{};runtime.Initialize(info.hw_stage,info.sw_stage);
    runtime.hw.cs.workgroup_size={threads,1,1};runtime.hw.cs.shared_memory_size=memory?256:0;
    Common::ObjectPool<IR::Inst> pool;IR::Block block(pool);IR::IREmitter ir(block);
    ir.WriteShared(32,ir.Imm32(7),ir.Imm32(0));
    const auto loaded=ir.LoadShared(32,false,ir.Imm32(0));
    ir.WriteShared(32,loaded,ir.Imm32(4));ir.Epilogue();
    IR::Program program(info);program.blocks.push_back(&block);program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};profile.supported_spirv=0x00010600;profile.needs_lds_barriers=needs;
    Optimization::CollectShaderInfoPass(program,profile);
    Optimization::SharedMemoryBarrierPass(program,runtime,profile);
    const bool subgroup=threads!=64 || !needs;
    CHECK(Count(block,IR::Opcode::SubgroupBarrier)==(memory && subgroup?3:0));
    CHECK(Count(block,IR::Opcode::Barrier)==(memory && !subgroup?3:0));
    Optimization::DeadCodeEliminationPass(program);
    CHECK(Count(block,subgroup?IR::Opcode::SubgroupBarrier:IR::Opcode::Barrier)==(memory?3:0));
    if(!memory) return;
    Backend::Bindings bindings{};const auto code=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
    std::unordered_map<u32,u32> constants;unsigned controls=0,memories=0;
    for(size_t i=5;i<code.size();) {
        const u32 n=code[i]>>16;CHECK(n && n<=code.size()-i);const auto op=spv::Op(code[i]&0xffff);
        if(op==spv::Op::OpConstant && n==4) constants[code[i+2]]=code[i+3];
        if(op==spv::Op::OpControlBarrier) ++controls;
        if(op==spv::Op::OpMemoryBarrier) {
            ++memories;CHECK(constants.at(code[i+1])==u32(spv::Scope::Subgroup));
            CHECK(constants.at(code[i+2])==u32(spv::MemorySemanticsMask::AcquireRelease|spv::MemorySemanticsMask::WorkgroupMemory));
        }
        i+=n;
    }
    CHECK(controls==(subgroup?0:3) && memories==(subgroup?3:0));
    Save(dir/("shared-"+std::to_string(threads)+"-"+std::to_string(needs)+".spv"),code);
}
static void CheckDivergence(bool loop) {
    Info info{};info.hw_stage=HwStage::Compute;info.sw_stage=SwStage::Compute;
    RuntimeInfo runtime{};runtime.Initialize(info.hw_stage,info.sw_stage);
    runtime.hw.cs.workgroup_size={64,1,1};runtime.hw.cs.shared_memory_size=256;
    Common::ObjectPool<IR::Inst> pool;IR::Block head(pool),body(pool),merge(pool);
    IR::IREmitter ir(head);
    const auto cond=ir.IEqual(ir.GetAttributeU32(IR::Attribute::LocalInvocationId),ir.Imm32(0));
    IR::IREmitter work(body);work.WriteShared(32,work.Imm32(7),work.Imm32(0));
    const auto loaded=work.LoadShared(32,false,work.Imm32(0));
    work.WriteShared(32,loaded,work.Imm32(4));
    IR::IREmitter end(merge);end.Epilogue();
    IR::Program program(info);using Type=IR::AbstractSyntaxNode::Type;
    program.blocks={&head,&body,&merge};
    program.syntax_list.push_back({.data={.block=&head},.type=Type::Block});
    if(loop) program.syntax_list.push_back({.data={.loop={&body,&body,&merge}},.type=Type::Loop});
    else program.syntax_list.push_back({.data={.if_node={cond,&body,&merge}},.type=Type::If});
    program.syntax_list.push_back({.data={.block=&body},.type=Type::Block});
    if(loop) program.syntax_list.push_back({.data={.repeat={cond,&body,&merge}},.type=Type::Repeat});
    else program.syntax_list.push_back({.data={.end_if={&merge}},.type=Type::EndIf});
    program.syntax_list.push_back({.data={.block=&merge},.type=Type::Block});
    Profile profile{};profile.needs_lds_barriers=true;
    Optimization::SharedMemoryBarrierPass(program,runtime,profile);
    CHECK(Count(body,IR::Opcode::Barrier)==0 && Count(merge,IR::Opcode::Barrier)==1);
}
int main(int argc,char**argv) {
    const std::filesystem::path dir=argc>1?argv[1]:".";
    CheckFp64(dir);
    for(u32 threads:{32u,64u,128u}) for(bool needs:{false,true}) CheckBarriers(threads,needs,true,dir);
    CheckBarriers(64,true,false,dir);CheckDivergence(false);CheckDivergence(true);
    std::puts("PASS: subgroup/workgroup ordering, side-effect retention, no-memory guard and divergent if/loop safety");
}
