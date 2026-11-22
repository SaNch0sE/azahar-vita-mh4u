// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <deque>
#include <utility>
#include <vector>
#include "common/assert.h"
#include "common/common_types.h"

namespace SgxdRenderer {

/**
 * Values by a 64-bit key that is a hash already (a program's): open addressing in a power-of-two
 * table, the slot from the key's own bits under a mask, so a lookup takes no division (the
 * standard unordered_map's prime bucket count cost an __aeabi_uidivmod at every draw). The values
 * stay where they are put (a deque), so a reference to one outlives later insertions; only Clear
 * removes them.
 */
template <typename V>
class KeyIndex {
public:
    [[nodiscard]] V* Find(u64 key) {
        if (slots.empty()) {
            return nullptr;
        }
        const u32 mask = static_cast<u32>(slots.size()) - 1;
        for (u32 i = Mix(key) & mask;; i = (i + 1) & mask) {
            const Slot& slot = slots[i];
            if (slot.value == 0) {
                return nullptr;
            }
            if (slot.key == key) {
                return &values[slot.value - 1];
            }
        }
    }

    /// A key not present yet.
    V& Emplace(u64 key, V&& value) {
        DEBUG_ASSERT(Find(key) == nullptr);
        if ((values.size() + 1) * 2 > slots.size()) {
            Grow();
        }
        values.push_back(std::move(value));
        Place(key, static_cast<u32>(values.size()));
        return values.back();
    }

    /// Each value, in the order put.
    template <typename F>
    void ForEach(F&& f) const {
        for (const V& value : values) {
            f(value);
        }
    }

    [[nodiscard]] u32 Size() const {
        return static_cast<u32>(values.size());
    }

    void Clear() {
        slots.clear();
        values.clear();
    }

private:
    struct Slot {
        u64 key;
        u32 value; ///< the value's index + 1; 0: an empty slot
    };

    static u32 Mix(u64 key) {
        return static_cast<u32>(key ^ (key >> 32));
    }

    void Place(u64 key, u32 value) {
        const u32 mask = static_cast<u32>(slots.size()) - 1;
        u32 i = Mix(key) & mask;
        while (slots[i].value != 0) {
            i = (i + 1) & mask;
        }
        slots[i] = Slot{key, value};
    }

    void Grow() {
        std::vector<Slot> old(slots.empty() ? 64 : slots.size() * 2);
        old.swap(slots);
        for (const Slot& slot : old) {
            if (slot.value != 0) {
                Place(slot.key, slot.value);
            }
        }
    }

    std::vector<Slot> slots;
    std::deque<V> values;
};

} // namespace SgxdRenderer
