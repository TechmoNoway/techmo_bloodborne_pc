// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/compile_inputs.h"
#include "shader_cache_version.h"
#include "common/hash.h"
#include <xxhash.h>
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace Shader::Cache {
std::string CompilerKey() {
    return std::string{TECHMO_SHADER_COMPILER_KEY} +
        (EmulatorSettings.IsDirectMemoryAccessEnabled() ? "-dma1" : "-dma0");
}

std::vector<u8> Inputs::Encode() const {
    Serialization::Archive ar;
    Serialization::Writer w{ar};
    w.Write(u32{0x49435354}); w.Write(u32{3});
    w.Write(std::string{TECHMO_SHADER_INPUT_ABI}); w.Write(compiler);
    w.Write(hw); w.Write(sw); w.Write(hash); w.Write(permutation); w.Write(program_base);
    auto persistent_runtime = runtime;
    if (hw == HwStage::Geometry) persistent_runtime.hw.gs.vs_copy = {};
    w.Write(persistent_runtime); w.Write(binding); w.Write(user_data.data(), sizeof(user_data)); w.Write(code);
    w.Write(copy_shader);
    w.Write(metadata_hash);
    w.Write(memory.flat); w.Write(memory.walker); w.Write(memory.fetch);
    w.Write(bool(memory.tess)); if (memory.tess) w.Write(*memory.tess);
    w.Write(memory.reads.size());
    for (const auto& r : memory.reads) { w.Write(r.base); w.Write(r.offset); w.Write(r.bytes); }
    auto bytes = ar.TakeOff();
    const u64 checksum = XXH3_64bits(bytes.data(), bytes.size());
    const auto* tail = reinterpret_cast<const u8*>(&checksum);
    bytes.insert(bytes.end(), tail, tail + sizeof(checksum));
    return bytes;
}

Inputs Inputs::Decode(std::vector<u8> bytes) {
    if (bytes.size() < 16 || bytes.size() > MaxInputBytes)
        throw Serialization::CorruptData{"shader input size"};
    u64 checksum{}; std::memcpy(&checksum, bytes.data() + bytes.size() - 8, 8);
    bytes.resize(bytes.size() - 8);
    if (checksum != XXH3_64bits(bytes.data(), bytes.size()))
        throw Serialization::CorruptData{"shader input checksum"};
    Serialization::Archive ar{std::move(bytes)};
    Serialization::Reader r{ar};
    u32 magic{}, version{}; std::string abi;
    r.Read(magic); r.Read(version); r.Read(abi);
    if (magic != 0x49435354 || version != 3 || abi != TECHMO_SHADER_INPUT_ABI)
        throw Serialization::CorruptData{"shader input ABI"};
    Inputs v;
    r.Read(v.compiler); r.Read(v.hw); r.Read(v.sw); r.Read(v.hash);
    r.Read(v.permutation); r.Read(v.program_base); r.Read(v.runtime); r.Read(v.binding);
    r.Read(v.user_data.data(), sizeof(v.user_data)); r.Read(v.code);
    r.Read(v.copy_shader);
    if (v.hw == HwStage::Geometry) v.runtime.hw.gs.vs_copy = {};
    r.Read(v.metadata_hash);
    r.Read(v.memory.flat); r.Read(v.memory.walker); r.Read(v.memory.fetch);
    bool tess{}; r.Read(tess); if (tess) { v.memory.tess.emplace(); r.Read(*v.memory.tess); }
    size_t count{}; r.Read(count);
    if (count > 512 || v.code.empty() || v.code.size() > 262144 || v.copy_shader.size() > 262144 ||
        v.memory.flat.size() < 16 || v.memory.flat.size() > 262144 ||
        v.memory.walker.size() > 1048576 || v.permutation >= 4096 ||
        u32(v.hw) > u32(HwStage::Compute) || u32(v.sw) > u32(SwStage::Compute) ||
        v.runtime.hw_stage != v.hw || v.runtime.sw_stage != v.sw)
        throw Serialization::CorruptData{"shader input bounds"};
    for (size_t i = 0; i < count; ++i) {
        auto& read = v.memory.reads.emplace_back();
        r.Read(read.base); r.Read(read.offset); r.Read(read.bytes);
        if (read.bytes.size() > 4096) throw Serialization::CorruptData{"shader input read size"};
    }
    if (!ar.IsEoS()) throw Serialization::CorruptData{"shader input trailing bytes"};
    v.memory.replay = true;
    return v;
}

void SaveInputs(const Inputs& inputs) {
    auto data = inputs.memory.valid ? inputs.Encode() : std::vector<u8>{};
    if (data.size() > MaxInputBytes) data.clear(); // Tombstone an older incompatible capture.
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderInputs,
        fmt::format("{:#018x}", HashCombine(inputs.hash, inputs.permutation)), std::move(data));
}
} // namespace Shader::Cache
