// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/build_id.h"

extern "C" {
// The note's ends (citra_vita's ldscript.ld); absent from an executable linked without it.
extern const char __build_id_start[] __attribute__((weak));
extern const char __build_id_end[] __attribute__((weak));
}

namespace Common {

std::span<const u8> BuildId() {
    // The note: the sizes of its name and ID, its type, the name to four bytes, the ID.
    if (__build_id_start == nullptr || __build_id_end - __build_id_start < 16) {
        return {};
    }
    const auto* const note = reinterpret_cast<const u32*>(__build_id_start);
    const auto* const end = reinterpret_cast<const u8*>(__build_id_end);
    const u32 id_size = note[1];
    const u8* const id = reinterpret_cast<const u8*>(note + 3) + ((note[0] + 3) & ~3u);
    if (id + id_size > end) {
        return {};
    }
    return {id, id_size};
}

} // namespace Common
