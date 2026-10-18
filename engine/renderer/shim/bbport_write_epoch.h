// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <limits>

// Host writes through the unprotected backing view bypass guest page faults.
// Overlapping writers must never make an in-progress write appear quiescent.
class BbBackingWriteEpoch {
public:
    static constexpr std::uint64_t Invalid = std::numeric_limits<std::uint64_t>::max();
    class Write {
    public:
        explicit Write(BbBackingWriteEpoch& epoch) : epoch{epoch} {
            epoch.writers.fetch_add(1, std::memory_order_seq_cst);
            epoch.version.fetch_add(1, std::memory_order_seq_cst);
        }
        ~Write() {
            epoch.version.fetch_add(1, std::memory_order_seq_cst);
            epoch.writers.fetch_sub(1, std::memory_order_seq_cst);
        }
        Write(const Write&) = delete;
        Write& operator=(const Write&) = delete;
    private:
        BbBackingWriteEpoch& epoch;
    };
    std::uint64_t Snapshot() const {
        const auto before = version.load(std::memory_order_seq_cst);
        if (writers.load(std::memory_order_seq_cst)) return Invalid;
        const auto after = version.load(std::memory_order_seq_cst);
        return before == after ? after : Invalid;
    }
private:
    std::atomic<std::uint64_t> version{0}, writers{0};
};
