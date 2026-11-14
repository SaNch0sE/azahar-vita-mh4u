// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <cstring>
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/renderer_gxm/gxm_program_disk_cache.h"

namespace GxmRenderer {

namespace {

constexpr u32 FileMagic = 0x4350484D;   // "MHPC"
constexpr u32 RecordMagic = 0x5250484D; // "MHPR"
constexpr u32 FormatVersion = 1;
constexpr std::size_t VersionBytes = 48;
/// Programs are a few KiB; anything larger is a damaged record.
constexpr u32 MaxProgram = 64 * 1024;
/// The card is shared with the saves: past this the cache stops growing.
constexpr u32 MaxFile = 16 * 1024 * 1024;

struct FileHeader {
    u32 magic;
    u32 format;
    char version[VersionBytes];
};

struct RecordHeader {
    u32 magic;
    u32 kind;
    u64 key;
    u32 input_regs;
    u32 size;
    u64 check;
};
static_assert(sizeof(RecordHeader) == 32);

u32 Padded(u32 size) {
    return (size + 3) & ~3u;
}

} // Anonymous namespace

ProgramDiskCache::ProgramDiskCache(std::string path_, std::string version_)
    : path{std::move(path_)}, version{std::move(version_)} {
    version.resize(VersionBytes - 1);
    if (!ReadIndex()) {
        StartOver();
    }
    if (usable) {
        writer = std::thread([this] { WriterLoop(); });
    }
}

ProgramDiskCache::~ProgramDiskCache() {
    {
        std::scoped_lock lock{mutex};
        quit = true;
    }
    wake.notify_all();
    if (writer.joinable()) {
        writer.join();
    }
    if (reader != nullptr) {
        std::fclose(reader);
    }
}

bool ProgramDiskCache::ReadIndex() {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    FileHeader header{};
    if (std::fread(&header, sizeof(header), 1, f) != 1 || header.magic != FileMagic ||
        header.format != FormatVersion ||
        std::strncmp(header.version, version.c_str(), VersionBytes) != 0) {
        LOG_INFO(Render, "program disk cache: {} is from another build, starting over", path);
        std::fclose(f);
        return false;
    }
    std::vector<u8> scratch;
    u32 offset = sizeof(header);
    for (;;) {
        RecordHeader record{};
        if (std::fread(&record, sizeof(record), 1, f) != 1) {
            break;
        }
        if (record.magic != RecordMagic || record.kind > 3 || record.size == 0 ||
            record.size > MaxProgram) {
            break;
        }
        const u32 body = Padded(record.size);
        // The record is indexed only once its whole body is known to be there: a run that
        // ended mid-write leaves a short last record, which is dropped.
        if (std::fseek(f, static_cast<long>(body), SEEK_CUR) != 0) {
            break;
        }
        const u32 data_offset = offset + static_cast<u32>(sizeof(record));
        if (std::ftell(f) != static_cast<long>(data_offset + body)) {
            break;
        }
        index[record.kind].insert_or_assign(
            record.key, Entry{data_offset, record.size, record.input_regs, record.check});
        offset = data_offset + body;
        loaded++;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);
    if (size != static_cast<long>(offset)) {
        // A damaged or cut-off tail: appending after it would hide every later record from
        // the next run's scan. Rare enough (a crash mid-write) that starting over is fine.
        LOG_WARNING(Render, "program disk cache: damaged after {} bytes of {}, starting over",
                    offset, size);
        for (auto& map : index) {
            map.clear();
        }
        loaded = 0;
        return false;
    }
    file_end = offset;
    reader = std::fopen(path.c_str(), "rb");
    usable = reader != nullptr;
    LOG_INFO(Render, "program disk cache: {} programs from {}", loaded, path);
    return true;
}

void ProgramDiskCache::StartOver() {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        LOG_WARNING(Render, "program disk cache: cannot create {}", path);
        return;
    }
    FileHeader header{};
    header.magic = FileMagic;
    header.format = FormatVersion;
    std::strncpy(header.version, version.c_str(), VersionBytes - 1);
    const bool ok = std::fwrite(&header, sizeof(header), 1, f) == 1;
    std::fclose(f);
    if (!ok) {
        return;
    }
    file_end = sizeof(header);
    reader = std::fopen(path.c_str(), "rb");
    usable = reader != nullptr;
}

std::optional<ProgramDiskCache::Program> ProgramDiskCache::Find(bool vertex, u64 key) {
    if (!usable) {
        return std::nullopt;
    }
    const Kind tiers[2] = {vertex ? Kind::Vertex2 : Kind::Fragment2,
                           vertex ? Kind::Vertex : Kind::Fragment};
    for (const Kind kind : tiers) {
        const auto& map = index[static_cast<u32>(kind)];
        const auto it = map.find(key);
        if (it == map.end()) {
            continue;
        }
        if (auto program = Read(it->second, kind)) {
            hits++;
            return program;
        }
    }
    return std::nullopt;
}

std::optional<ProgramDiskCache::Program> ProgramDiskCache::Read(const Entry& entry, Kind kind) {
    Program program;
    program.bytes.resize(entry.size);
    if (std::fseek(reader, static_cast<long>(entry.offset), SEEK_SET) != 0 ||
        std::fread(program.bytes.data(), 1, entry.size, reader) != entry.size) {
        return std::nullopt;
    }
    if (Common::ComputeHash64(program.bytes.data(), program.bytes.size()) != entry.check) {
        LOG_WARNING(Render, "program disk cache: a program at {} fails its check", entry.offset);
        return std::nullopt;
    }
    program.input_regs = entry.input_regs;
    program.tier = kind == Kind::Fragment2 || kind == Kind::Vertex2 ? 2 : 1;
    return program;
}

void ProgramDiskCache::Store(Kind kind, u64 key, std::span<const u8> program, u32 input_regs) {
    if (!usable || program.empty() || program.size() > MaxProgram ||
        index[static_cast<u32>(kind)].contains(key)) {
        return;
    }
    {
        std::scoped_lock lock{mutex};
        pending.push_back(Pending{kind, key, input_regs, {program.begin(), program.end()}});
    }
    wake.notify_one();
}

void ProgramDiskCache::WriterLoop() {
    Common::SetCurrentThreadName("ProgramDiskCache");
    // Core 3 at the lowest priority, with the tier-2 worker: the card can take its time.
    Common::SetCurrentThreadRole(Common::ThreadRole::Background);
    u32 end = file_end;
    std::vector<Pending> batch;
    for (;;) {
        {
            std::unique_lock lock{mutex};
            wake.wait(lock, [this] { return quit || !pending.empty(); });
            // A new area asks for its programs in a burst: one open of the file for all of them.
            wake.wait_for(lock, std::chrono::milliseconds(500), [this] { return quit; });
            if (pending.empty() && quit) {
                return;
            }
            batch.swap(pending);
        }
        std::FILE* f = std::fopen(path.c_str(), "ab");
        if (f == nullptr) {
            batch.clear();
            continue;
        }
        for (const Pending& p : batch) {
            const u32 size = static_cast<u32>(p.program.size());
            if (end + sizeof(RecordHeader) + Padded(size) > MaxFile) {
                break;
            }
            RecordHeader record{};
            record.magic = RecordMagic;
            record.kind = static_cast<u32>(p.kind);
            record.key = p.key;
            record.input_regs = p.input_regs;
            record.size = size;
            record.check = Common::ComputeHash64(p.program.data(), p.program.size());
            static constexpr u8 zeros[4]{};
            if (std::fwrite(&record, sizeof(record), 1, f) != 1 ||
                std::fwrite(p.program.data(), 1, size, f) != size ||
                std::fwrite(zeros, 1, Padded(size) - size, f) != Padded(size) - size) {
                break;
            }
            end += static_cast<u32>(sizeof(record)) + Padded(size);
        }
        std::fclose(f);
        batch.clear();
    }
}

} // namespace GxmRenderer
