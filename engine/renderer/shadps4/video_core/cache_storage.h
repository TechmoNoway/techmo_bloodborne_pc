// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <atomic>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
    ShaderInputs,
    ShaderCompiler,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    // Offline diagnostics: no worker, writes, migration or cache invalidation.
    void OpenReadOnly(const std::filesystem::path& directory);
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    [[nodiscard]] bool IsReadOnly() const { return read_only_directory; }
    void FinishPreload();
    const std::filesystem::path& Path() const { return cache_path; }
    /// bbport: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);
    std::vector<std::string> ListBlobs(BlobType type) const;

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::atomic<bool> opened{};
    bool read_only_directory{};
};

} // namespace Storage
