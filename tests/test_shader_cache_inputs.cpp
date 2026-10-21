// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/compile_inputs.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "common/hash.h"
#include "common/serdes.h"
#include "video_core/cache_file.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include <cassert>
#include <crtdbg.h>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <xxhash.h>
extern "C" int bbgpu_rebuild_shader_cache(const char*);

static std::vector<u8> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

// Compare active typed fields, never static_vector's unused slots, struct padding
// or the relocated address of a host SRT walker. Raw metadata is not canonical.
static void CheckPersistentInfo(const Shader::Info& a, const Shader::Info& b) {
    assert(a.buffers.size() == b.buffers.size() && a.images.size() == b.images.size() &&
           a.samplers.size() == b.samplers.size() && a.fmasks.size() == b.fmasks.size());
    for (size_t i = 0; i < a.buffers.size(); ++i) {
        const auto& x = a.buffers[i]; const auto& y = b.buffers[i];
        assert(x.sharp_fetch == y.sharp_fetch);
        const auto fields = [](const auto& v) { return std::tuple{v.used_types, v.buffer_type,
            v.is_written, v.is_formatted, v.post_op, v.post_op_dw1_mask}; };
        assert(fields(x) == fields(y));
    }
    for (size_t i = 0; i < a.images.size(); ++i) {
        const auto& x = a.images[i]; const auto& y = b.images[i];
        assert(x.sharp_fetch == y.sharp_fetch);
        const auto fields = [](const auto& v) { return std::tuple{v.is_depth, v.is_atomic,
            v.is_array, v.is_written, v.is_r128, v.needs_native, v.constant_mip_index,
            v.mip_fallback_mode, v.post_op}; };
        assert(fields(x) == fields(y));
    }
    for (size_t i = 0; i < a.samplers.size(); ++i) {
        const auto& x = a.samplers[i]; const auto& y = b.samplers[i];
        assert(x.sharp_fetch == y.sharp_fetch && x.post_op == y.post_op &&
               x.post_op_tsharp_dw3_off == y.post_op_tsharp_dw3_off && x.is_depth == y.is_depth);
    }
    for (size_t i = 0; i < a.fmasks.size(); ++i) assert(a.fmasks[i].sharp_idx == b.fmasks[i].sharp_idx);
    const auto fields = [](const auto& v) { return std::tuple{v.ud_mask.mask,
        v.fetch_shader_sgpr_base, v.shared_memory_scratch_size, v.pgm_hash,
        v.tess_consts_dword_offset, v.tess_consts_ptr_base, v.hw_stage, v.sw_stage,
        v.mrt_mask, v.has_fetch_shader, v.has_bitwise_xor, v.uses_dma}; };
    assert(fields(a) == fields(b) && a.flattened_ud_buf == b.flattened_ud_buf);
    assert(a.srt_info.flattened_bufsize_dw == b.srt_info.flattened_bufsize_dw &&
           a.srt_info.walker_func_size == b.srt_info.walker_func_size);
    if (a.srt_info.walker_func_size) assert(!std::memcmp(
        reinterpret_cast<const void*>(a.srt_info.walker_func),
        reinterpret_cast<const void*>(b.srt_info.walker_func), a.srt_info.walker_func_size));
}

static void VerifyCapturedLookups(const std::filesystem::path& root,
                                  const std::filesystem::path& reference = {}) {
    using namespace Shader;
    const auto bytes = Read(root / "profile.bin");
    assert(bytes.size() == sizeof(Profile));
    Profile profile{}; std::memcpy(&profile, bytes.data(), sizeof(profile));
    unsigned checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".input") continue;
        auto input = Cache::Inputs::Decode(Read(entry.path()));
        const auto expected = HashCombine(input.hash, input.permutation);
        Serialization::Archive ar{Read(root / (entry.path().stem().string() + ".meta"))};
        Info loaded{}; StageSpecialization spec; spec.info = &loaded;
        std::optional<Gcn::FetchShaderData> fetch; size_t permutation{};
        assert(Vulkan::LoadShaderMeta(ar, loaded, fetch, spec, permutation, expected));
        if (!reference.empty()) {
            Serialization::Archive other_ar{Read(reference / (entry.path().stem().string() + ".meta"))};
            Info other{}; StageSpecialization other_spec; other_spec.info = &other;
            size_t other_permutation{};
            assert(Vulkan::LoadShaderMeta(other_ar, other, fetch, other_spec, other_permutation, expected));
            assert(other_permutation == permutation && other_spec == spec && spec == other_spec &&
                   other_spec.start == spec.start && other_spec.bitset == spec.bitset);
            CheckPersistentInfo(loaded, other);
        }
        // Compare the persisted payload before RefreshFlatBuf can replace it with
        // the captured request's data, then check the actual runtime lookup path.
        loaded.compile_memory = &input.memory;
        loaded.user_data = input.user_data;
        loaded.pgm_base = input.program_base;
        loaded.RefreshFlatBuf();
        const StageSpecialization lookup{loaded, input.runtime, profile, input.binding};
        loaded.compile_memory = nullptr;
        assert(spec == lookup);
        ++checked;
    }
    assert(checked > 0);
    std::printf("PASS: %u captured shader lookup keys match their rebuilt metadata\n", checked);
    if (!reference.empty()) std::printf("PASS: %u typed shader metadata records match reference\n", checked);
}

static void CheckClipRebuild(const std::filesystem::path& root) {
    using namespace Shader;
    std::filesystem::create_directories(root);
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.needs_clip_distance_emulation = true;
    Cache::Inputs input;
    input.compiler = "previous-translator";
    input.hash = 0x912341;
    input.hw = HwStage::Fragment; input.sw = SwStage::Fragment;
    input.runtime.Initialize(input.hw, input.sw);
    input.runtime.hw.fs.clip_distance_emulation = true;
    input.runtime.hw.fs.num_inputs = 6;
    for (u32 i = 0; i < 6; ++i) input.runtime.hw.fs.inputs[i].param_index = i;
    input.code = {0xBEEB03FF, 0, 0xBF810000};
    Info info{input.hw, input.sw, ShaderParams{input.user_data, input.code, input.hash}};
    info.compile_memory = &input.memory;
    Pools pools; auto translated = input.runtime; auto bindings = input.binding;
    const auto ir = TranslateProgram(input.code, pools, info, translated, profile);
    assert(translated.hw.fs.num_inputs == 7);
    const auto original = Backend::SPIRV::EmitSPIRV(profile, translated, ir, bindings);
    // Reproduce the old record: it persisted the compiler's extra input in its lookup key.
    const StageSpecialization old_spec{info, translated, profile, input.binding};
    const auto metadata = Cache::Metadata(info, old_spec, 0);
    input.metadata_hash = XXH3_64bits(metadata.data(), metadata.size());
    info.compile_memory = nullptr;
    const auto name = fmt::format("{:#018x}", HashCombine(input.hash, size_t{0}));
    assert(Storage::ReplaceCacheFile(root / "profile.bin", {reinterpret_cast<const u8*>(&profile), sizeof(profile)}));
    assert(Storage::ReplaceCacheFile(root / (name + ".input"), input.Encode()));
    assert(Storage::ReplaceCacheFile(root / (name + ".meta"), metadata));
    std::error_code error;
    std::filesystem::remove(root / (name + ".attempt"), error);
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    Serialization::Archive ar{Read(root / (name + ".meta"))};
    Info loaded{}; StageSpecialization spec; spec.info = &loaded;
    std::optional<Gcn::FetchShaderData> fetch; size_t permutation{};
    assert(Vulkan::LoadShaderMeta(ar, loaded, fetch, spec, permutation, HashCombine(input.hash, size_t{0})));
    assert(spec.runtime_info.hw.fs.num_inputs == 6);
    auto snapshot = Cache::Inputs::Decode(Read(root / (name + ".input")));
    loaded.compile_memory = &snapshot.memory;
    loaded.user_data = snapshot.user_data;
    loaded.RefreshFlatBuf();
    const StageSpecialization lookup{loaded, snapshot.runtime, profile, snapshot.binding};
    assert(spec == lookup);
    loaded.compile_memory = nullptr;
    const auto binary = Read(root / fmt::format("{:#018x}_0.spv", input.hash));
    assert(binary.size() == original.size() * sizeof(u32));
    assert(!std::memcmp(binary.data(), original.data(), binary.size()));
    puts("PASS: clip-distance cache rebuild preserves requested lookup key and byte-identical GPU shader");
}

int main(int argc, char** argv) {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    using namespace Shader;
    if (argc == 3 && std::string_view{argv[1]} == "--verify-lookups") {
        VerifyCapturedLookups(argv[2]);
        return 0;
    }
    if (argc == 4 && std::string_view{argv[1]} == "--compare-lookups") {
        VerifyCapturedLookups(argv[2], argv[3]);
        return 0;
    }
    assert(argc == 2);
    const std::filesystem::path root(argv[1]);
    std::filesystem::create_directories(root);
    Cache::Inputs input;
    input.compiler = "previous-translator"; input.hash = 0x912340;
    input.hw = HwStage::Compute; input.sw = SwStage::Compute;
    input.runtime.Initialize(input.hw, input.sw); input.runtime.hw.cs.workgroup_size = {1,1,1};
    input.program_base = 0xdead0000;
    input.code = {0xBEEB03FF, 0, 0xBF810000};
    Info info{input.hw, input.sw, ShaderParams{input.user_data, input.code, input.hash}};
    info.compile_memory = &input.memory;
    Profile profile{}; profile.supported_spirv = 0x00010600; profile.support_int64 = true;
    Pools pools; auto runtime = input.runtime; auto binding = input.binding;
    const auto ir = TranslateProgram(input.code, pools, info, runtime, profile);
    const auto original = Backend::SPIRV::EmitSPIRV(profile, runtime, ir, binding);
    StageSpecialization spec{info, runtime, profile, input.binding};
    info.compile_memory = nullptr;
    const auto source_metadata = Cache::Metadata(info,spec,0);
    input.metadata_hash = XXH3_64bits(source_metadata.data(),source_metadata.size());
    auto bytes = input.Encode();
    auto restored = Cache::Inputs::Decode(bytes);
    assert(restored.memory.replay && restored.code == input.code);
    assert(restored.program_base == input.program_base && restored.memory.flat == input.memory.flat);
    auto geometry=input; geometry.hw=HwStage::Geometry; geometry.sw=SwStage::Geometry;
    geometry.runtime.Initialize(geometry.hw,geometry.sw);
    geometry.copy_shader={0xBEEB03FF,0,0xBF810000};
    geometry.runtime.hw.gs.vs_copy=geometry.copy_shader;
    auto owned_geometry=Cache::Inputs::Decode(geometry.Encode());
    assert(owned_geometry.copy_shader==geometry.copy_shader && owned_geometry.runtime.hw.gs.vs_copy.empty());
    auto truncated = bytes; truncated.pop_back();
    bool rejected = false;
    try { Cache::Inputs::Decode(std::move(truncated)); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
    bytes[24] ^= 1; rejected = false;
    try { Cache::Inputs::Decode(std::move(bytes)); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
    CompileMemory replay; replay.replay = true;
    Info missing{}; missing.compile_memory = &replay; u32 bad{};
    rejected = false;
    try { bad = missing.ReadUdReg<u32>(0, 0); } catch (const std::exception&) { rejected = true; }
    assert(rejected && bad == 0);
    missing.compile_memory = nullptr;
    CompileMemory changing;
    const u32 first=12, second=24;
    changing.CaptureRegister(0,0,&first,sizeof(first));
    changing.CaptureRegister(0,0,&second,sizeof(second));
    assert(!changing.valid); // A changing input must not be replayed as a coherent snapshot.
    const auto name = fmt::format("{:#018x}", HashCombine(input.hash, input.permutation));
    assert(Storage::ReplaceCacheFile(root / "profile.bin", {reinterpret_cast<const u8*>(&profile), sizeof(profile)}));
    assert(Storage::ReplaceCacheFile(root / (name + ".input"), input.Encode()));
    assert(Storage::ReplaceCacheFile(root / (name + ".meta"), source_metadata));
    std::error_code error; std::filesystem::remove(root / (name + ".attempt"), error);
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    const auto binary = Read(root / fmt::format("{:#018x}_0.spv", input.hash));
    assert(binary.size() == original.size() * 4 && !std::memcmp(binary.data(), original.data(), binary.size()));
    Serialization::Archive ar{Read(root / (name + ".meta"))};
    Info loaded{}; StageSpecialization loaded_spec; loaded_spec.info = &loaded;
    std::optional<Gcn::FetchShaderData> fetch; size_t permutation{};
    assert(Vulkan::LoadShaderMeta(ar, loaded, fetch, loaded_spec, permutation, HashCombine(input.hash, input.permutation)));
    assert(Cache::Inputs::Decode(Read(root / (name + ".input"))).compiler == Cache::CompilerKey());
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    assert(!Cache::NeedsRebuild(root));
    // A directory-wide current stamp must not hide an incomplete shader commit.
    std::filesystem::remove(root / (name + ".compiler"), error);
    assert(Cache::NeedsRebuild(root));
    const auto captured_current = Read(root / (name + ".input"));
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    assert(Read(root / (name + ".compiler")) == Read(root / "translator.compiler"));
    assert(Read(root / fmt::format("{:#018x}_0.spv", input.hash)) == binary);
    assert(Cache::Inputs::Decode(Read(root / (name + ".input"))).code ==
           Cache::Inputs::Decode(captured_current).code);
    assert(!Cache::NeedsRebuild(root));
    const std::array<u8, 3> stale_stamp{'o','l','d'};
    assert(Storage::ReplaceCacheFile(root / (name + ".compiler"), stale_stamp));
    assert(Cache::NeedsRebuild(root));
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    assert(!Cache::NeedsRebuild(root));
    puts("PASS: current global/input stamps do not hide missing or stale per-stage stamps; rebuilt SPIR-V is identical");
    assert(Storage::ReplaceCacheFile(root / (name + ".input"), input.Encode()));
    assert(Storage::ReplaceCacheFile(root / (name + ".meta"), source_metadata));
    const auto key = Cache::CompilerKey();
    assert(Storage::ReplaceCacheFile(root / (name + ".attempt"), {reinterpret_cast<const u8*>(key.data()), key.size()}));
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    assert(Cache::Inputs::Decode(Read(root / (name + ".input"))).compiler == "previous-translator");
    auto other_generation=source_metadata; other_generation[12]^=1;
    assert(Storage::ReplaceCacheFile(root/(name+".meta"),other_generation));
    std::filesystem::remove(root/(name+".attempt"),error);
    assert(bbgpu_rebuild_shader_cache(root.string().c_str())==0);
    assert(Read(root/(name+".meta"))==other_generation);
    assert(Cache::Inputs::Decode(Read(root/(name+".input"))).compiler=="previous-translator");
    // New attempt markers cover the capture generation, not only the compiler.
    assert(Storage::ReplaceCacheFile(root / (name + ".compiler"), stale_stamp));
    assert(!Cache::NeedsRebuild(root)); // Same failed input/metadata is suppressed.
    assert(Storage::ReplaceCacheFile(root / (name + ".meta"), source_metadata));
    assert(Cache::NeedsRebuild(root)); // Coherent replacement can be retried now.
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    assert(!Cache::NeedsRebuild(root));
    puts("PASS: unchanged failed generation is suppressed; repaired capture can retry in the same compiler build");
    // If the invalidation stamp cannot be installed, no binary/metadata/input
    // may be replaced under the previous compiler authentication.
    const auto stable_input = Read(root / (name + ".input"));
    const auto stable_metadata = Read(root / (name + ".meta"));
    const auto binary_path = root / fmt::format("{:#018x}_0.spv", input.hash);
    const std::array<u8, 3> damaged_binary{'b','a','d'};
    assert(Storage::ReplaceCacheFile(binary_path, damaged_binary));
    const auto stamp_path = root / (name + ".compiler");
    std::filesystem::remove(stamp_path, error);
    assert(std::filesystem::create_directory(stamp_path));
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    assert(Read(binary_path) == std::vector<u8>(damaged_binary.begin(), damaged_binary.end()));
    assert(Read(root / (name + ".input")) == stable_input && Read(root / (name + ".meta")) == stable_metadata);
    assert(std::filesystem::remove(stamp_path)); // Empty fixture directory only.
    std::filesystem::remove(root / (name + ".attempt"), error);
    assert(bbgpu_rebuild_shader_cache(root.string().c_str()) == 0);
    assert(Read(binary_path) == binary);
    puts("PASS: failed invalidation stamp leaves shader files untouched; successful repair authenticates only its complete file set");
    CheckClipRebuild(root / "clip-specialization");
    puts("PASS: owned inputs, corruption rejection, unmapped-pointer replay, GCN rebuild, metadata, warm hit and failed-entry isolation");
}
