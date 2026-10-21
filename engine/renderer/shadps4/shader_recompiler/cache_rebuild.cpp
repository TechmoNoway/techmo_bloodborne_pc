// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/compile_inputs.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/recompiler.h"
#include "common/hash.h"
#include "common/io_file.h"
#include "video_core/cache_file.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <xxhash.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Shader::Cache {
static std::vector<u8> Read(const std::filesystem::path& path, size_t limit = MaxInputBytes) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return {};
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen() || file.GetSize() > limit) return {};
    std::vector<u8> bytes(file.GetSize());
    if (file.Read(bytes) != bytes.size()) return {};
    return bytes;
}

static bool Write(const std::filesystem::path& path, std::span<const u8> bytes) {
    return Storage::ReplaceCacheFile(path, bytes);
}

static std::vector<u8> Attempt(const std::string& compiler, std::span<const u8> input,
                               std::span<const u8> metadata) {
    const auto value = fmt::format("{}:{:016x}:{:016x}", compiler,
        XXH3_64bits(input.data(), input.size()), XXH3_64bits(metadata.data(), metadata.size()));
    return {value.begin(), value.end()};
}

static bool ValidBinary(std::span<const u8> bytes) {
    if (bytes.size() < 20 || bytes.size() % sizeof(u32)) return false;
    const auto word = [&](size_t offset) { u32 v; std::memcpy(&v, bytes.data() + offset * 4, 4); return v; };
    if (word(0) != 0x07230203) return false;
    for (size_t i = 5; i < bytes.size() / 4;) {
        const u32 count = word(i) >> 16;
        if (!count || count > bytes.size() / 4 - i) return false;
        i += count;
    }
    return true;
}

static int Rebuild(const std::filesystem::path& directory) {
    // Profile is checked by the parent against the active GPU before this helper is launched.
    const auto profile_bytes = Read(directory / "profile.bin", sizeof(Profile));
    if (profile_bytes.size() != sizeof(Profile)) return 2;
    Profile profile{}; std::memcpy(&profile, profile_bytes.data(), sizeof(profile));
    const auto compiler = CompilerKey();
    const std::vector<u8> stamp(compiler.begin(), compiler.end());
    unsigned rebuilt = 0, rejected = 0, current = 0, uncaptured = 0;
    const auto started = std::chrono::steady_clock::now();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error) || entry.path().extension() != ".input" || entry.is_symlink(error)) continue;
        const auto name = entry.path().stem().string();
        const auto failed = directory / (name + ".attempt");
        const auto input_bytes = Read(entry.path());
        if (input_bytes.empty()) { ++uncaptured; continue; }
        const auto original_metadata = Read(directory / (name + ".meta"));
        const auto attempt = Attempt(compiler, input_bytes, original_metadata);
        try {
            auto input = Inputs::Decode(input_bytes);
            if (original_metadata.empty() || input.metadata_hash != XXH3_64bits(original_metadata.data(), original_metadata.size()))
                throw std::runtime_error("shader inputs belong to a different metadata generation");
            if (fmt::format("{:#018x}", HashCombine(input.hash, input.permutation)) != name)
                throw std::runtime_error("shader input identity mismatch");
            const auto binary = directory / fmt::format("{:#018x}_{}.spv", input.hash, input.permutation);
            if (input.compiler == compiler && Read(directory / (name + ".compiler"), 128) == stamp &&
                ValidBinary(Read(binary))) { ++current; continue; }
            const auto previous_attempt = Read(failed, 128);
            if (previous_attempt == stamp || previous_attempt == attempt) { ++rejected; continue; }
            // Persist before entering the translator: after an assertion/crash the parent can
            // restart the helper and the failing shader will not be attempted again this build.
            if (!Write(failed, attempt)) throw std::runtime_error("cannot save shader attempt");
            std::printf("Shader rebuild: %s\n", name.c_str()); std::fflush(stdout);
            Info info{input.hw, input.sw, ShaderParams{input.user_data, input.code, input.hash}};
            info.pgm_base = input.program_base; info.compile_memory = &input.memory;
            Pools pools;
            auto runtime = input.runtime;
            if (input.hw == HwStage::Geometry) runtime.hw.gs.vs_copy = input.copy_shader;
            auto bindings = input.binding;
            const auto ir = TranslateProgram(input.code, pools, info, runtime, profile);
            auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime, ir, bindings);
            // Use the original request for lookup, not compiler-added interface inputs.
            StageSpecialization spec{info, input.runtime, profile, input.binding};
            info.compile_memory = nullptr;
            auto metadata = Metadata(info, spec, input.permutation);
            input.compiler = compiler;
            input.metadata_hash = XXH3_64bits(metadata.data(), metadata.size());
            auto encoded = input.Encode();
            // Also repairing a same-compiler entry must be fail-closed: no old
            // current stamp may authenticate a partially replaced file set.
            constexpr std::array<u8, 7> pending{'p','e','n','d','i','n','g'};
            if (!Write(directory / (name + ".compiler"), pending) ||
                !Write(binary, {reinterpret_cast<const u8*>(spv.data()), spv.size() * sizeof(u32)}) ||
                !Write(directory / (name + ".meta"), metadata) ||
                !Write(entry.path(), encoded) || !Write(directory / (name + ".compiler"), stamp))
                throw std::runtime_error("cannot commit rebuilt shader");
            std::filesystem::remove(failed, error);
            ++rebuilt;
        } catch (const std::exception& e) {
            ++rejected;
            // Scope new failures to this capture/metadata generation. A fresh
            // capture may be retried without waiting for another compiler build.
            Write(failed, attempt);
            std::printf("Shader rebuild skipped: %s (%s)\n", name.c_str(), e.what());
        }
    }
    std::printf("Shader rebuild summary: %u rebuilt, %u current, %u skipped, %u uncaptured, %.2f ms\n",
        rebuilt, current, rejected, uncaptured, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    return Write(directory / "translator.compiler", stamp) ? 0 : 3;
}

bool NeedsRebuild(const std::filesystem::path& directory) {
    const auto compiler = CompilerKey();
    const auto previous = Read(directory / "translator.compiler", 128);
    if (std::string(previous.begin(), previous.end()) == compiler) {
        // The global stamp describes a scan, not every record's generation.
        // Interrupted writes and mixed caches can still contain repairable entries.
        const std::vector<u8> stamp(compiler.begin(), compiler.end());
        unsigned candidates = 0, suppressed = 0;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (entry.path().extension() != ".input" || !entry.is_regular_file(error) || entry.is_symlink(error)) continue;
            const auto size = entry.file_size(error);
            if (error || size < 16 || size > MaxInputBytes) continue;
            auto stage_stamp = entry.path(); stage_stamp.replace_extension(".compiler");
            if (Read(stage_stamp, 128) == stamp) continue;
            auto failed = entry.path(); failed.replace_extension(".attempt");
            const auto last_attempt = Read(failed, 128);
            if (!last_attempt.empty()) {
                auto metadata = entry.path(); metadata.replace_extension(".meta");
                if (last_attempt == stamp || last_attempt == Attempt(compiler, Read(entry.path()), Read(metadata))) {
                    ++suppressed; continue;
                }
            }
            ++candidates;
        }
        std::printf("Shader repair scan: %u candidates, %u previous failures unchanged\n", candidates, suppressed);
        if (!candidates) return false;
    }
    return true;
}

void RebuildBeforeWarmUp(const std::filesystem::path& directory) {
    const char* enabled = std::getenv("BB_SHADER_REBUILD");
    if (enabled && std::string_view{enabled} == "0") return;
    if (!NeedsRebuild(directory)) return;
#ifdef _WIN32
    std::array<wchar_t, 32768> executable{};
    if (!GetModuleFileNameW(nullptr, executable.data(), executable.size())) return;
    auto helper = std::filesystem::path(executable.data()).parent_path() / "bb-shader-lab.exe";
    if (!std::filesystem::is_regular_file(helper)) {
        std::puts("Shader rebuild: helper unavailable; regular compilation retained"); return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (unsigned restart = 0; restart < 8; ++restart) {
        std::wstring command = L"\"" + helper.wstring() + L"\" \"" + directory.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) break;
        CloseHandle(process.hThread);
        bool timed_out = false;
        while (WaitForSingleObject(process.hProcess, 100) == WAIT_TIMEOUT) {
            if (std::chrono::steady_clock::now() >= deadline) {
                TerminateProcess(process.hProcess, 124); WaitForSingleObject(process.hProcess, 5000);
                timed_out = true; break;
            }
        }
        DWORD status = 1; GetExitCodeProcess(process.hProcess, &status); CloseHandle(process.hProcess);
        if (status == 0) return;
        if (timed_out || status == 2 || status == 3) break;
        std::printf("Shader rebuild: helper exited %#lx; continuing past its last attempted shader\n", status);
    }
    std::puts("Shader rebuild incomplete; remaining shaders use regular compilation");
#endif
}
} // namespace Shader::Cache

extern "C" int bbgpu_rebuild_shader_cache(const char* directory) {
    try { return Shader::Cache::Rebuild(std::filesystem::u8path(directory)); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Shader rebuild: %s\n", error.what()); return 3;
    }
}
