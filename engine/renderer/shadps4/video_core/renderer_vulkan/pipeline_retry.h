// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan {
// Guarded by DrawPreparation's submission mutex. A publication during a pass
// remains observable: Complete must not consume the newer generation.
class PipelineRetry {
  public:
    static constexpr unsigned MaxPasses = 4;
    bool Claimable(std::uint64_t generation, bool allow_retry) const {
        return !busy && (passes == 0 || (allow_retry && missing && passes < MaxPasses &&
                                         generation > observed_generation));
    }
    bool FirstPass() const { return passes == 0; }
    void Claim(std::uint64_t generation) {
        busy = true;
        observed_generation = generation;
    }
    void Complete(bool has_missing_shader) {
        busy = false;
        missing = has_missing_shader;
        ++passes;
    }

  private:
    std::uint64_t observed_generation = 0;
    unsigned passes = 0;
    bool busy = false, missing = false;
};
} // namespace Vulkan
