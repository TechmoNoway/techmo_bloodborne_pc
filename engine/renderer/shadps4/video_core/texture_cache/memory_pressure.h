// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <algorithm>

namespace VideoCore {

// Driver budgets are estimates and change as other applications use the GPU.
// Separate entry/exit marks keep an eviction burst from oscillating at one mark.
class MemoryPressure {
public:
    enum class Level { Normal, Pressure, Critical };
    struct Thresholds {
        std::uint64_t trigger{}, pressure{}, critical{}, pressure_exit{}, critical_exit{};
    };

    static constexpr std::uint64_t Percent(std::uint64_t bytes, unsigned percent) {
        return (bytes / 100) * percent + (bytes % 100) * percent / 100;
    }

    void Update(std::uint64_t usage, std::uint64_t budget) {
        if (!budget) return; // A temporarily unavailable budget is not a zero-sized heap.
        // Reclaim long-unused area textures even when VRAM is plentiful. Waiting for
        // 70% of a large heap first would retain old maps unnecessarily.
        marks = {std::min(Percent(budget, 70), std::uint64_t{512} << 20),
                 Percent(budget, 85), Percent(budget, 95),
                 Percent(budget, 80), Percent(budget, 90)};
        if (usage >= marks.critical) {
            level = Level::Critical;
        } else if (level == Level::Critical && usage >= marks.critical_exit) {
            // Keep critical reclamation until enough headroom has returned.
        } else if (usage >= marks.pressure ||
                   (level != Level::Normal && usage >= marks.pressure_exit)) {
            level = Level::Pressure;
        } else {
            level = Level::Normal;
        }
    }

    Thresholds marks{};
    Level level = Level::Normal;
};

} // namespace VideoCore
