// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <limits>
#include <initializer_list>
#include "video_core/texture_cache/memory_pressure.h"

int main() {
    using VideoCore::MemoryPressure;
    using Level = MemoryPressure::Level;
    constexpr std::uint64_t GiB = 1ull << 30;
    // Realistic low-memory cards: every threshold must remain within the budget.
    for (auto budget : {GiB / 2, GiB, 4 * GiB, 6 * GiB, 8 * GiB}) {
        MemoryPressure p;
        p.Update(0, budget);
        const auto m = p.marks;
        assert(m.trigger <= 512ull * 1024 * 1024);
        assert(m.trigger < m.pressure_exit && m.pressure_exit < m.pressure);
        assert(m.pressure < m.critical_exit && m.critical_exit < m.critical);
        assert(m.critical < budget);
        p.Update(m.pressure, budget);
        assert(p.level == Level::Pressure);
        for (int i = 0; i < 10; ++i) {
            p.Update(m.pressure - 1, budget); // Oscillating around entry must not flip state.
            assert(p.level == Level::Pressure);
            p.Update(m.pressure, budget);
        }
        p.Update(m.pressure_exit, budget);
        assert(p.level == Level::Pressure);
        p.Update(m.pressure_exit - 1, budget);
        assert(p.level == Level::Normal);
        p.Update(m.critical, budget);
        assert(p.level == Level::Critical);
        p.Update(m.critical_exit, budget);
        assert(p.level == Level::Critical);
        p.Update(m.critical_exit - 1, budget);
        assert(p.level == Level::Pressure);
        p.Update(m.pressure_exit - 1, budget);
        assert(p.level == Level::Normal);
    }
    MemoryPressure p;
    p.Update(3 * GiB, 8 * GiB);
    assert(p.level == Level::Normal);
    p.Update(3 * GiB, 3 * GiB); // Another application reduces the driver budget.
    assert(p.level == Level::Critical);
    const auto critical = p.marks.critical;
    p.Update(0, 0); // Unavailable query must preserve the last valid policy.
    assert(p.level == Level::Critical && p.marks.critical == critical);
    p.Update(3 * GiB, 8 * GiB);
    assert(p.level == Level::Normal);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    assert(MemoryPressure::Percent(max, 100) == max);
    assert(MemoryPressure::Percent(max, 95) < max);
    // Against an independent wide-integer reference; no multiply-before-divide overflow.
    for (unsigned percent : {70u, 80u, 85u, 90u, 95u}) {
        for (auto bytes : {std::uint64_t{101}, 4 * GiB + 37, max}) {
            assert(MemoryPressure::Percent(bytes, percent) ==
                   std::uint64_t((__uint128_t(bytes) * percent) / 100));
        }
    }
    puts("PASS: memory pressure, shrinking budgets, recovery hysteresis and low-VRAM thresholds");
}
