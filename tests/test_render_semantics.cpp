// SPDX-License-Identifier: GPL-2.0-or-later
// Production translation and entry-point policies; no game/cache required.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sirit/sirit.h>
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"FAIL render semantics %d: %s\n",__LINE__,#x);std::_Exit(1); } } while(false)
using namespace Shader;
namespace Shader::Gcn {
struct RenderSemanticsTest {
    static void Fetch(Translator& translator) {
        translator.fetch_data.emplace();
        translator.fetch_data->vertex_offset_sgpr=0;
        translator.fetch_data->instance_offset_sgpr=1;
    }
};
}
static u32 Evaluate(const IR::Value& value) {
    if(value.IsImmediate()) return value.U32();
    const auto* inst=value.Inst();
    switch(inst->GetOpcode()) {
    case IR::Opcode::GetAttributeU32:
        switch(inst->Arg(0).Attribute()) {
        case IR::Attribute::VertexId:return 48;
        case IR::Attribute::BaseVertex:return 41;
        case IR::Attribute::InstanceId:return 22;
        case IR::Attribute::BaseInstance:return 13;
        default:CHECK(false);
        }
        break;
    case IR::Opcode::ISub32:return Evaluate(inst->Arg(0))-Evaluate(inst->Arg(1));
    case IR::Opcode::UDiv32:return Evaluate(inst->Arg(0))/Evaluate(inst->Arg(1));
    default:CHECK(false);
    }
    return 0;
}
static void CheckIndirect() {
    unsigned cases=0;
    for(auto hw:{HwStage::Vertex,HwStage::Export,HwStage::Local}) {
        for(unsigned mode=0;mode<5;++mode) {
            const bool indirect=mode>=2,sgpr=mode>=3,fetch=mode==1||mode==4;
            Info info{};info.hw_stage=hw;info.sw_stage=SwStage::Vertex;
            RuntimeInfo runtime{};runtime.Initialize(hw,info.sw_stage);
            runtime.props.num_input_vgprs=3;runtime.props.num_user_data=sgpr?2:0;
            runtime.sw.vs.step_rate_0=2;runtime.sw.vs.step_rate_1=3;runtime.sw.vs.is_indirect=indirect;
            const u32 offset=hw==HwStage::Vertex?0x4c:hw==HwStage::Export?0xcc:0x14c;
            runtime.sw.vs.vertex_sgpr_offset=sgpr?offset:0;
            runtime.sw.vs.instance_sgpr_offset=sgpr?offset+1:0;
            Common::ObjectPool<IR::Inst> pool;IR::Block block(pool);
            Profile profile{};Gcn::Translator translator(info,runtime,profile);
            if(fetch) Gcn::RenderSemanticsTest::Fetch(translator);
            translator.EmitPrologue(&block);
            std::array<u32,4> values{};std::array<bool,4> seen{};
            for(const auto& inst:block.Instructions()) if(inst.GetOpcode()==IR::Opcode::SetVectorRegister) {
                const auto reg=u32(inst.Arg(0).VectorReg());
                if(reg<values.size()) { values[reg]=Evaluate(inst.Arg(1));seen[reg]=true; }
            }
            CHECK(seen[0] && values[0]==(indirect&&!fetch?7:48));
            if(hw!=HwStage::Local) {
                const u32 instance=indirect&&!fetch?9:22;
                CHECK(seen[1] && values[1]==instance/2);
                CHECK(seen[2] && values[2]==instance/3);
                CHECK(seen[3] && values[3]==instance);
            }
            auto other=runtime;other.sw.vs.is_indirect=!indirect;CHECK(!(runtime==other));
            ++cases;
        }
    }
    std::printf("PASS: %u direct/indirect prologues, absent SGPRs, fetch-owned offsets, local/export stages and instance step rates\n",cases);
}
static void CheckDepth(const std::filesystem::path& dir) {
    for(bool early:{false,true}) for(bool depth:{false,true}) for(bool discard:{false,true}) {
        Info info{};info.hw_stage=HwStage::Fragment;info.sw_stage=SwStage::Fragment;
        RuntimeInfo runtime{};runtime.Initialize(info.hw_stage,info.sw_stage);
        runtime.hw.fs.depth_before_shader=early;
        runtime.hw.fs.color_buffers[0].num_format=AmdGpu::NumberFormat::Float;
        auto other=runtime;other.hw.fs.depth_before_shader=!early;CHECK(!(runtime==other));
        Common::ObjectPool<IR::Inst> pool;IR::Block block(pool);IR::IREmitter ir(block);
        for(u32 i=0;i<4;++i) ir.SetAttribute(IR::Attribute::RenderTarget0,ir.Imm32(i==3?1.0f:0.25f),i);
        if(depth) ir.SetAttribute(IR::Attribute::Depth,ir.Imm32(0.5f));
        if(discard) ir.Discard(ir.Imm1(false));
        ir.Epilogue();IR::Program program(info);program.blocks.push_back(&block);program.post_order_blocks.push_back(&block);
        program.syntax_list.push_back({.data={.block=&block},.type=IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.type=IR::AbstractSyntaxNode::Type::Return});
        Profile profile{};profile.supported_spirv=0x00010600;
        Optimization::CollectShaderInfoPass(program,profile);
        Backend::Bindings bindings{};const auto code=Backend::SPIRV::EmitSPIRV(profile,runtime,program,bindings);
        bool has_early=false,has_depth=false;
        for(size_t i=5;i<code.size();) {
            const u32 count=code[i]>>16;CHECK(count && count<=code.size()-i);
            if(spv::Op(code[i]&0xffff)==spv::Op::OpExecutionMode) {
                has_early|=code[i+2]==u32(spv::ExecutionMode::EarlyFragmentTests);
                has_depth|=code[i+2]==u32(spv::ExecutionMode::DepthReplacing);
            }
            i+=count;
        }
        CHECK(has_early==(early&&!depth) && has_depth==depth);
        const auto path=dir/("fragment-"+std::to_string(early)+"-"+std::to_string(depth)+"-"+std::to_string(discard)+".spv");
        std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(code.data()),code.size()*sizeof(u32));CHECK(bool(output));
    }
    puts("PASS: guest-only early tests, preserved depth exports/discard, specialization separation; eight SPIR-V variants");
}
int main(int argc,char**argv) {
    const std::filesystem::path dir=argc>1?argv[1]:".";
    CheckIndirect();CheckDepth(dir);
}
