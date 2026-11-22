// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "core/hle/kernel/host_link.h"
#include "core/hle/kernel/object.h"

namespace Kernel {

HostInbox::HostInbox() {
    for (u32 i = 0; i < Size; i++) {
        cells[i].seq.store(i, std::memory_order_relaxed);
    }
}

void HostInbox::Push(HostMessage&& message) {
    u32 pos = tail.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = cells[pos & (Size - 1)];
        const u32 seq = cell.seq.load(std::memory_order_acquire);
        const s32 diff = static_cast<s32>(seq - pos);
        if (diff == 0) {
            if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.message = std::move(message);
                cell.seq.store(pos + 1, std::memory_order_release);
                break;
            }
        } else {
            ASSERT_MSG(diff > 0, "a host's inbox is full ({} messages)", Size);
            pos = tail.load(std::memory_order_relaxed);
        }
    }
    // The sleeper publishes itself before it checks the ring: one of the two sees the other.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleeping.load(std::memory_order_relaxed)) {
        flag.Raise();
    }
}

bool HostInbox::Pop(HostMessage& out) {
    const u32 h = head.load(std::memory_order_relaxed);
    Cell& cell = cells[h & (Size - 1)];
    if (cell.seq.load(std::memory_order_acquire) != h + 1) {
        return false;
    }
    out = std::move(cell.message);
    cell.message.object.reset();
    cell.seq.store(h + Size, std::memory_order_release);
    head.store(h + 1, std::memory_order_seq_cst);
    return true;
}

bool HostInbox::Empty() const {
    const u32 h = head.load(std::memory_order_relaxed);
    return cells[h & (Size - 1)].seq.load(std::memory_order_acquire) != h + 1;
}

bool HostInbox::Pending() const {
    // A slot claimed counts (its message is on the way).
    return tail.load(std::memory_order_seq_cst) != head.load(std::memory_order_seq_cst);
}

void HostInbox::Wake() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleeping.load(std::memory_order_relaxed)) {
        flag.Raise();
    }
}

} // namespace Kernel
