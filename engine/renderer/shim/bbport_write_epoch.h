// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Host writes through the unprotected backing view bypass guest page faults.
// Overlapping writers must never make an in-progress write appear quiescent.
class BbBackingWriteEpoch {
    struct Node {
        std::atomic<std::uint64_t> version{0}, writers{0};
        void Begin() { writers.fetch_add(1, std::memory_order_seq_cst); version.fetch_add(1, std::memory_order_seq_cst); }
        void End() { version.fetch_add(1, std::memory_order_seq_cst); writers.fetch_sub(1, std::memory_order_seq_cst); }
        std::uint64_t Read() const {
            const auto before = version.load(std::memory_order_seq_cst);
            if (writers.load(std::memory_order_seq_cst)) return Invalid;
            const auto after = version.load(std::memory_order_seq_cst);
            return before == after ? after : Invalid;
        }
    };
    static constexpr std::size_t StripeCount = 1024, MaxRangeStripes = 64;
    static constexpr unsigned StripeShift = 20;
    struct Range { std::size_t first{}, count{}; };
    static Range Locate(std::uint64_t address, std::uint64_t size) {
        if (!size || address > Invalid - size) return {};
        const auto first = address >> StripeShift;
        const auto count = ((address + size - 1) >> StripeShift) - first + 1;
        if (count > MaxRangeStripes) return {};
        return {std::size_t(first % StripeCount), std::size_t(count)};
    }
public:
    static constexpr std::uint64_t Invalid = std::numeric_limits<std::uint64_t>::max();
    class Write {
    public:
        explicit Write(BbBackingWriteEpoch& epoch) : Write{epoch, 0, 0} {}
        Write(BbBackingWriteEpoch& epoch, std::uint64_t address, std::uint64_t size)
            : epoch{epoch}, range{Locate(address, size)} {
            epoch.global.Begin();
            if (!range.count) epoch.unscoped.Begin();
            for (std::size_t i = 0; i < range.count; ++i)
                epoch.stripes[(range.first + i) % StripeCount].Begin();
        }
        ~Write() {
            for (std::size_t i = 0; i < range.count; ++i)
                epoch.stripes[(range.first + i) % StripeCount].End();
            if (!range.count) epoch.unscoped.End();
            epoch.global.End();
        }
        Write(const Write&) = delete;
        Write& operator=(const Write&) = delete;
    private:
        BbBackingWriteEpoch& epoch;
        Range range;
    };
    std::uint64_t Snapshot() const { return global.Read(); }
    std::uint64_t Snapshot(std::uint64_t address, std::uint64_t size) const {
        const auto range = Locate(address, size);
        if (!range.count) return Snapshot();
        const auto read = [&] {
            auto total = unscoped.Read();
            if (total == Invalid) return Invalid;
            for (std::size_t i = 0; i < range.count; ++i) {
                const auto value = stripes[(range.first + i) % StripeCount].Read();
                if (value == Invalid || total >= Invalid - value) return Invalid;
                total += value;
            }
            return total;
        };
        // Each component only increases: a sum cannot hide a changed stripe.
        // Unknown/oversized writes invalidate all ranges; stripe aliases only
        // cause conservative extra hashing. Overflow disables reuse.
        const auto before = read();
        return before != Invalid && before == read() ? before : Invalid;
    }
private:
    Node global, unscoped;
    std::array<Node, StripeCount> stripes{};
};
