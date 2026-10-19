// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <vector>
#include <type_traits>

#include "common/types.h"

namespace Common {

template <typename ObjectType, typename TickType>
class LeastRecentlyUsedCache {
    static constexpr size_t None = std::numeric_limits<size_t>::max();
    struct Item {
        ObjectType obj;
        TickType tick;
        size_t next = None;
        size_t prev = None;
    };

public:
    LeastRecentlyUsedCache() = default;
    ~LeastRecentlyUsedCache() = default;

    size_t Insert(ObjectType obj, TickType tick) {
        const auto new_id = Build();
        auto& item = item_pool[new_id];
        item.obj = obj;
        item.tick = tick;
        Attach(new_id);
        return new_id;
    }

    void Touch(size_t id, TickType tick) {
        auto& item = item_pool[id];
        if (item.tick >= tick) {
            return;
        }
        item.tick = tick;
        if (id == last_item) {
            return;
        }
        Detach(id);
        Attach(id);
    }

    void Free(size_t id) {
        Detach(id);
        free_items.push_back(id);
    }

    template <typename Func>
    void ForEachItemBelow(TickType tick, Func&& func) {
        static constexpr bool RETURNS_BOOL =
            std::is_same_v<std::invoke_result_t<Func&, ObjectType&>, bool>;
        size_t iterator = first_item;
        while (iterator != None) {
            if (item_pool[iterator].tick > tick) {
                return;
            }
            const size_t next = item_pool[iterator].next;
            if constexpr (RETURNS_BOOL) {
                if (func(item_pool[iterator].obj)) {
                    return;
                }
            } else {
                func(item_pool[iterator].obj);
            }
            iterator = next;
        }
    }

private:
    size_t Build() {
        if (free_items.empty()) {
            const size_t item_id = item_pool.size();
            item_pool.emplace_back();
            return item_id;
        }
        const size_t item_id = free_items.back();
        free_items.pop_back();
        auto& item = item_pool[item_id];
        item.next = None;
        item.prev = None;
        return item_id;
    }

    void Attach(size_t id) {
        auto& item = item_pool[id];
        item.prev = last_item;
        item.next = None;
        if (last_item != None) item_pool[last_item].next = id;
        else first_item = id;
        last_item = id;
    }

    void Detach(size_t id) {
        auto& item = item_pool[id];
        if (item.prev != None) item_pool[item.prev].next = item.next;
        else first_item = item.next;
        if (item.next != None) item_pool[item.next].prev = item.prev;
        else last_item = item.prev;
        item.prev = item.next = None;
    }

    // Indices survive contiguous storage growth, including callbacks that free
    // the current item. Keep IDs stable for image/sampler relocation safety.
    std::vector<Item> item_pool;
    std::vector<size_t> free_items;
    size_t first_item = None;
    size_t last_item = None;
};

} // namespace Common
