// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/common_types.h"

namespace GxmRenderer {

/**
 * Emitted GXM programs kept on the memory card between runs.
 *
 * The USSE emitters take 1-10 ms a program on the render thread, and a new area asks for a
 * few dozen at once: walking out of the village spawn emitted ~30 in 0.7 s, the render thread
 * fell behind, the GX queue's brake held the emulation side, and the game dropped to 20 fps for
 * two seconds (2026-11-14). With the programs on disk, only the first visit ever pays.
 *
 * Keys are the in-memory cache's own (fs_hash, the vertex key), so a program read back is the
 * one the emitter would have produced - as long as the emitter is the same. The file carries a
 * version made of a hash of the emitter's sources (generated at build time) and, in the
 * diagnostic build, of the flags file, and a file with another version is started over.
 *
 * The index (key -> offset) is read at startup; programs are read when first asked for.
 * New programs are written by a worker thread, so the render thread never waits on the card.
 * Render thread only, apart from the worker.
 */
class ProgramDiskCache {
public:
    enum class Kind : u32 { Fragment = 0, Vertex = 1, Fragment2 = 2, Vertex2 = 3 };

    struct Program {
        std::vector<u8> bytes;
        u32 input_regs = 0;
        int tier = 1;
    };

    ProgramDiskCache(std::string path, std::string version);
    ~ProgramDiskCache();

    /// The best program on disk for a key: the second tier's when there is one.
    std::optional<Program> Find(bool vertex, u64 key);

    /// Queues a program for writing (nothing happens if the key is already on disk).
    void Store(Kind kind, u64 key, std::span<const u8> program, u32 input_regs);

    u32 Loaded() const {
        return loaded;
    }
    u32 Hits() const {
        return hits;
    }

private:
    struct Entry {
        u32 offset;
        u32 size;
        u32 input_regs;
        u64 check;
    };
    struct Pending {
        Kind kind;
        u64 key;
        u32 input_regs;
        std::vector<u8> program;
    };

    bool ReadIndex();
    void StartOver();
    std::optional<Program> Read(const Entry& entry, Kind kind);
    void WriterLoop();

    std::string path;
    std::string version;
    std::FILE* reader = nullptr;
    std::unordered_map<u64, Entry> index[4];
    u32 file_end = 0;
    u32 loaded = 0;
    u32 hits = 0;
    bool usable = false;

    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Pending> pending;
    bool quit = false;
    std::thread writer;
};

} // namespace GxmRenderer
