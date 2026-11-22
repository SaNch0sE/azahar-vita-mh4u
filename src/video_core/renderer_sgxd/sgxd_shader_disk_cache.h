// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <optional>
#include "common/common_types.h"
#include "video_core/renderer_sgxd/usse/usse_program.h"

namespace SgxdRenderer {

/**
 * The second tier's programs on the memory card, a file a version:
 * ux0:data/azahar/shaders/TITLE.KIND.VERSION/programs, a record a program, each starting with its
 * byte count and ending with its checksum. The second tier's workers keep the programs they emit in
 * memory (Store) and write them in batches (WriteBatch): every operation on the card is one the
 * title's own reads wait behind. A file whose records end before it does (a stop cut its last write
 * short, or the card damaged a record) is cut back to them. After Open, a loader thread reads the
 * file's programs into memory kept for the process (a savestate load's new rasterizer finds them
 * there, and the programs stored since); lookups (Find, Contains) read memory only, so no drawing
 * thread waits on the card, and a program not read yet is a miss.
 *
 * The version is the executable's build ID (Common::BuildId, the linker's hash of what it
 * linked) with the diagnostics build's sgxd_flags: each version has its own directory, so no build
 * runs the programs another one emitted. Other versions' directories stay on the card.
 */
class ShaderDiskCache {
public:
    /// Render thread, before its first lookup: the version's directory made and the loader
    /// started. Once: later calls do nothing. With use_disk_shader_cache
    /// off, or before Open, nothing is found or stored.
    void Open(u64 title_id);

    struct Entry {
        Usse::Program program;
        u32 input_regs; ///< a vertex program's
    };
    /// The second tier's program for the key, when a run stored it and it is read. Any thread.
    [[nodiscard]] std::optional<Entry> Find(u64 key, bool vertex) const;
    /// Whether Find has the key's program. Any thread.
    [[nodiscard]] bool Contains(u64 key, bool vertex) const;
    /// Keeps a second tier program for the next batch, unless the file has it. Any thread.
    void Store(u64 key, bool vertex, const Usse::Program& program, u32 input_regs);
    /// Writes the programs stored since the last batch once they fill one or the oldest has
    /// waited ten seconds; `all`: now. Returns the us until that, 0 with nothing to write. The
    /// second tier's workers, and its end.
    u32 WriteBatch(bool all = false);

    /// The versions' directories, TITLE.KIND.VERSION each: TITLE the program id in 16 hex digits.
    static constexpr const char* Root = "ux0:data/azahar/shaders";
    /// No title running, its directories about to be deleted: the programs a run of the title
    /// read or stored, kept for the process, dropped, so that its next Open reads the card again.
    static void Forget(u64 title_id);

private:
    bool opened = false;            ///< Open's own
    std::atomic<bool> ready{false}; ///< a version's directory open (Open's release)
};

} // namespace SgxdRenderer
