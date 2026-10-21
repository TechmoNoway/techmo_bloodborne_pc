// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include "shader_recompiler/compile_memory.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/specialization.h"

namespace Shader::Cache {
inline constexpr size_t MaxInputBytes = 4 * 1024 * 1024;
struct Inputs {
    std::string compiler;
    HwStage hw{};
    SwStage sw{};
    u64 hash{}, permutation{}, program_base{};
    u64 metadata_hash{};
    RuntimeInfo runtime{};
    Backend::Bindings binding{};
    std::array<u32, 16> user_data{};
    std::vector<u32> code;
    std::vector<u32> copy_shader;
    CompileMemory memory;

    std::vector<u8> Encode() const;
    static Inputs Decode(std::vector<u8> bytes);
};
std::string CompilerKey();
std::vector<u8> Metadata(const Info& info, const StageSpecialization& spec, size_t permutation);
void SaveInputs(const Inputs& inputs);
bool NeedsRebuild(const std::filesystem::path& directory);
void RebuildBeforeWarmUp(const std::filesystem::path& directory);
} // namespace Shader::Cache
