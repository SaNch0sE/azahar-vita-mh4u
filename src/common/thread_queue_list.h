// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <tuple>
#include <boost/serialization/deque.hpp>
#include <boost/serialization/split_member.hpp>
#include "common/common_types.h"

namespace Common {

template <class T, unsigned int N>
struct ThreadQueueList {
    // TODO(yuriks): If performance proves to be a problem, the std::deques can be replaced with
    //               (dynamically resizable) circular buffers to remove their overhead when
    //               inserting and popping.

    using Priority = unsigned int;

    // Number of priority levels. (Valid levels are [0..NUM_QUEUES).)
    static constexpr Priority NUM_QUEUES = N;
    static_assert(N <= 64, "one bit a level in `nonempty`");

    ThreadQueueList() {
        first = nullptr;
    }

    // Only for debugging, returns priority level.
    [[nodiscard]] Priority contains(const T& uid) const {
        for (Priority i = 0; i < NUM_QUEUES; ++i) {
            const Queue& cur = queues[i];
            if (std::find(cur.data.cbegin(), cur.data.cend(), uid) != cur.data.cend()) {
                return i;
            }
        }

        return -1;
    }

    // The first thread of the most urgent level that has one: the lowest set bit of `nonempty`, not
    // a walk over every level ever used.
    [[nodiscard]] T get_first() const {
        return nonempty != 0 ? queues[std::countr_zero(nonempty)].data.front() : T();
    }

    std::pair<Priority, T> pop_first() {
        return PopLevelIn(nonempty);
    }

    std::pair<Priority, T> pop_first_better(Priority priority) {
        return PopLevelIn(nonempty & ((u64{1} << priority) - 1));
    }

    void push_front(Priority priority, const T& thread_id) {
        Queue* cur = &queues[priority];
        cur->data.push_front(thread_id);
        nonempty |= u64{1} << priority;
    }

    void push_back(Priority priority, const T& thread_id) {
        Queue* cur = &queues[priority];
        cur->data.push_back(thread_id);
        nonempty |= u64{1} << priority;
    }

    void move(const T& thread_id, Priority old_priority, Priority new_priority) {
        remove(old_priority, thread_id);
        prepare(new_priority);
        push_back(new_priority, thread_id);
    }

    void remove(Priority priority, const T& thread_id) {
        Queue* const cur = &queues[priority];
        const auto iter = std::remove(cur->data.begin(), cur->data.end(), thread_id);
        cur->data.erase(iter, cur->data.end());
        if (cur->data.empty()) {
            nonempty &= ~(u64{1} << priority);
        }
    }

    void rotate(Priority priority) {
        Queue* cur = &queues[priority];

        if (cur->data.size() > 1) {
            cur->data.push_back(std::move(cur->data.front()));
            cur->data.pop_front();
        }
    }

    void clear() {
        queues.fill(Queue());
        first = nullptr;
        nonempty = 0;
    }

    [[nodiscard]] bool empty(Priority priority) const {
        const Queue* cur = &queues[priority];
        return cur->data.empty();
    }

    /// Each thread with its level, most urgent level first, each level front to back.
    template <typename F>
    void for_each(F&& f) const {
        for (u64 levels = nonempty; levels != 0; levels &= levels - 1) {
            const Priority priority = static_cast<Priority>(std::countr_zero(levels));
            for (const T& t : queues[priority].data) {
                f(priority, t);
            }
        }
    }

    void prepare(Priority priority) {
        Queue* cur = &queues[priority];
        if (cur->next_nonempty == UnlinkedTag())
            link(priority);
    }

private:
    struct Queue {
        // Points to the next active priority, skipping over ones that have never been used.
        Queue* next_nonempty = UnlinkedTag();
        // Double-ended queue of threads in this priority level
        std::deque<T> data;
    };

    /// The first thread of the most urgent level among `levels` (bits of non-empty levels).
    std::pair<Priority, T> PopLevelIn(u64 levels) {
        if (levels == 0) {
            return {Priority(), T()};
        }
        const Priority prio = static_cast<Priority>(std::countr_zero(levels));
        Queue& cur = queues[prio];
        auto tmp = std::move(cur.data.front());
        cur.data.pop_front();
        if (cur.data.empty()) {
            nonempty &= ~(u64{1} << prio);
        }
        return {prio, tmp};
    }

    /// Special tag used to mark priority levels that have never been used.
    static Queue* UnlinkedTag() {
        return reinterpret_cast<Queue*>(1);
    }

    void link(Priority priority) {
        Queue* cur = &queues[priority];

        for (int i = priority - 1; i >= 0; --i) {
            if (queues[i].next_nonempty != UnlinkedTag()) {
                cur->next_nonempty = queues[i].next_nonempty;
                queues[i].next_nonempty = cur;
                return;
            }
        }

        cur->next_nonempty = first;
        first = cur;
    }

    // The first queue that's ever been used.
    Queue* first;
    // Bit p set while level p holds a thread.
    u64 nonempty = 0;
    // The priority level queues of thread ids.
    std::array<Queue, NUM_QUEUES> queues;

    s64 ToIndex(const Queue* q) const {
        if (q == nullptr) {
            return -2;
        } else if (q == UnlinkedTag()) {
            return -1;
        } else {
            return q - queues.data();
        }
    }

    Queue* ToPointer(s64 idx) {
        if (idx == -1) {
            return UnlinkedTag();
        } else if (idx < 0) {
            return nullptr;
        } else {
            return &queues[idx];
        }
    }

    friend class boost::serialization::access;
    template <class Archive>
    void save(Archive& ar, const unsigned int file_version) const {
        const s64 idx = ToIndex(first);
        ar << idx;
        for (std::size_t i = 0; i < NUM_QUEUES; i++) {
            const s64 idx1 = ToIndex(queues[i].next_nonempty);
            ar << idx1;
            ar << queues[i].data;
        }
    }

    template <class Archive>
    void load(Archive& ar, const unsigned int file_version) {
        s64 idx;
        ar >> idx;
        first = ToPointer(idx);
        for (std::size_t i = 0; i < NUM_QUEUES; i++) {
            ar >> idx;
            queues[i].next_nonempty = ToPointer(idx);
            ar >> queues[i].data;
        }
        nonempty = 0;
        for (std::size_t i = 0; i < NUM_QUEUES; i++) {
            if (!queues[i].data.empty()) {
                nonempty |= u64{1} << i;
            }
        }
    }

    BOOST_SERIALIZATION_SPLIT_MEMBER()
};

} // namespace Common
