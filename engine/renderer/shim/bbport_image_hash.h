// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace BbImageHash {
struct Range { std::size_t offset, size; };
std::uint64_t Serial(const void* data, std::size_t size);
// Optional large-image experiment. Workers only see an owned readable snapshot;
// they never fault on guest pages while the texture-cache mutex is held.
// False leaves output untouched and asks the caller to use the normal path.
bool SnapshotParallel(const void* data, std::size_t size, std::span<const Range> mips,
                      std::span<std::uint64_t> output);
}
