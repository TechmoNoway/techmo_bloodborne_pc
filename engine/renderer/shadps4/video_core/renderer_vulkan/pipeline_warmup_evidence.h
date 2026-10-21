// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <string_view>
#include <unordered_map>

namespace Vulkan {
// Startup-only writes; runtime misses consult this immutable diagnostic map.
// It does not control pipeline validity or suppress any required compilation.
class PipelineWarmupEvidence {
public:
    struct Result { std::string_view state, rejection; };
    void Reset(std::string_view state) { session = state; records.clear(); }
    void Record(const std::string& name, bool loaded, std::string_view rejection) {
        auto [it, inserted] = records.try_emplace(name, loaded ? "loaded" : rejection);
        // A rejected alias must not hide an already loaded canonical key.
        if (!inserted && (loaded || it->second != "loaded"))
            it->second = loaded ? "loaded" : rejection;
    }
    Result Lookup(const std::string& name) const {
        if (session != "ready") return {session, "none"};
        const auto it = records.find(name);
        if (it == records.end()) return {"absent", "none"};
        return it->second == "loaded" ? Result{"loaded", "none"} : Result{"rejected", it->second};
    }
private:
    std::string_view session = "not_run";
    std::unordered_map<std::string, std::string_view> records;
};
}
