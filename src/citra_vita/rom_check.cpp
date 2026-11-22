// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include <cryptopp/sha.h>
#include <fmt/format.h>
#include <psp2/kernel/processmgr.h>

#include "citra_vita/i18n.h"
#include "citra_vita/rom_check.h"
#include "common/file_derived.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "common/thread.h"
#include "core/file_sys/cia_container.h"
#include "core/file_sys/ncch_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/loader/loader.h"

namespace VitaFrontend::RomCheck {

namespace {

using I18n::Translatable;

constexpr u64 MediaUnit = 0x200;
constexpr std::size_t ChunkSize = 1 << 20;
using Digest = std::array<u8, CryptoPP::SHA256::DIGESTSIZE>;

/// A part's name for the menus: the NCSD partition's or the CIA content's index.
const char* PartName(u32 index) {
    switch (index) {
    case 0:
        return Translatable("the game");
    case 1:
        return Translatable("the manual");
    case 2:
        return Translatable("Download Play");
    case 6:
    case 7:
        return Translatable("the update data");
    default:
        return Translatable("extra content");
    }
}

/// A chunk's blocks hashed and compared on four cores: three workers (cores 0, 1 and 3, idle while
/// the game list shows) and the caller, which reads the next chunk meanwhile. One core hashes
/// slower than the memory card reads.
class BlockHasher {
public:
    BlockHasher() {
        for (int i = 0; i < 3; i++) {
            workers.emplace_back(
                Common::ThreadCfg{"file check", 32 * 1024, 1 << 0 | 1 << 1 | 1 << 3},
                [this] { Run(); });
        }
    }

    ~BlockHasher() {
        {
            std::scoped_lock lock{gate};
            stop = true;
            gate.NotifyAll();
        }
        workers.clear();
    }

    /// Starts on `count` blocks of `block` bytes at `data`, each against its digest in
    /// `expected`. The caller keeps both until Wait returns.
    void Start(const u8* data, u64 block, u64 count, const u8* expected) {
        std::scoped_lock lock{gate};
        job = {data, block, count, expected};
        next.store(0, std::memory_order_relaxed);
        finished = 0;
        first_bad = -1;
        generation++;
        gate.NotifyAll();
    }

    /// Hashes beside the workers until the chunk is done: the first block that differs, or -1.
    s64 Wait() {
        Job taken;
        {
            std::scoped_lock lock{gate};
            taken = job;
            active++;
        }
        Help(taken);
        std::scoped_lock lock{gate};
        while (finished < job.count || active != 0) {
            gate.Wait();
        }
        return first_bad;
    }

private:
    struct Job {
        const u8* data = nullptr;
        u64 block = 0;
        u64 count = 0;
        const u8* expected = nullptr;
    };

    void Run() {
        u64 seen = 0;
        for (;;) {
            Job taken;
            {
                std::scoped_lock lock{gate};
                while (generation == seen && !stop) {
                    gate.Wait();
                }
                if (stop) {
                    return;
                }
                seen = generation;
                // A chunk already done (this worker woke after the caller's Wait): taking it would
                // take the next chunk's first index once Start resets the count.
                if (finished >= job.count) {
                    continue;
                }
                taken = job;
                active++;
            }
            Help(taken);
        }
    }

    /// Blocks of `taken` until none is left; leaves the job (active) when done.
    void Help(const Job& taken) {
        u64 done = 0;
        s64 bad = -1;
        for (u64 i; (i = next.fetch_add(1, std::memory_order_relaxed)) < taken.count;) {
            Digest digest;
            CryptoPP::SHA256().CalculateDigest(digest.data(), taken.data + i * taken.block,
                                               static_cast<std::size_t>(taken.block));
            if (std::memcmp(digest.data(), taken.expected + i * 32, 32) != 0 &&
                (bad < 0 || static_cast<s64>(i) < bad)) {
                bad = static_cast<s64>(i);
            }
            done++;
        }
        std::scoped_lock lock{gate};
        finished += done;
        if (bad >= 0 && (first_bad < 0 || bad < first_bad)) {
            first_bad = bad;
        }
        active--;
        gate.NotifyAll();
    }

    Common::CoreCondition gate;
    Job job;
    std::atomic<u64> next{0};
    u64 finished = 0;
    s64 first_bad = -1;
    u64 generation = 0;
    int active = 0;
    bool stop = false;
    std::vector<Common::NamedThread> workers;
};

/// The file being checked, its read buffer, and the progress.
class Checker {
public:
    Checker(FileUtil::IOFileBase& file_, const std::function<void(float)>& progress_, bool quick_)
        : file{file_}, size{file_.GetSize()}, progress{progress_}, quick{quick_} {}

    u64 Size() const {
        return size;
    }

    /// QuickCheck's: the headers, the game's ExeFS and its RomFS's top hash level, and every
    /// part's place in the file; not the data blocks.
    bool Quick() const {
        return quick;
    }

    void SetTotal(u64 bytes) {
        total = std::max<u64>(bytes, 1);
    }

    bool Read(u64 offset, std::size_t length, u8* out) {
        return offset + length <= size && file.ReadAtBytes(out, length, offset) == length;
    }

    /// The SHA-256 of `length` bytes at `offset`; none where the file ends first.
    std::optional<Digest> Hash(u64 offset, u64 length) {
        if (offset + length > size) {
            return std::nullopt;
        }
        CryptoPP::SHA256 sha;
        buffer.resize(ChunkSize);
        for (u64 done = 0; done < length;) {
            const std::size_t n = static_cast<std::size_t>(std::min<u64>(ChunkSize, length - done));
            if (!Read(offset + done, n, buffer.data())) {
                return std::nullopt;
            }
            sha.Update(buffer.data(), n);
            done += n;
            Advance(n);
        }
        Digest digest;
        sha.Final(digest.data());
        return digest;
    }

    /// Each `block`-byte block of `length` bytes at `offset` (the last padded with zeros) against
    /// its digest, the parents' `first`-th onwards from `parents`. The first block that differs,
    /// or none; a read past the file's end is block -1.
    std::optional<s64> Level(u64 offset, u64 length, u64 block,
                             const std::function<bool(u64 first, u64 count, u8* out)>& parents) {
        const u64 blocks = (length + block - 1) / block;
        const u64 per_chunk = std::max<u64>(1, ChunkSize / block);
        if (!hasher) {
            hasher.emplace();
        }
        // A chunk read and its digests fetched while the one before is hashed.
        std::array<std::vector<u8>, 2> chunks, digests;
        std::optional<u64> started;
        u64 started_bytes = 0;
        const auto finish = [&]() -> std::optional<s64> {
            if (started) {
                if (const s64 bad = hasher->Wait(); bad >= 0) {
                    return static_cast<s64>(*started) + bad;
                }
                Advance(started_bytes);
                started.reset();
            }
            return std::nullopt;
        };
        int cur = 0;
        for (u64 b = 0; b < blocks; b += per_chunk) {
            const u64 count = std::min(per_chunk, blocks - b);
            const u64 bytes = std::min(count * block, length - b * block);
            chunks[cur].assign(static_cast<std::size_t>(count * block), 0);
            digests[cur].resize(static_cast<std::size_t>(count * 32));
            const bool read =
                Read(offset + b * block, static_cast<std::size_t>(bytes), chunks[cur].data()) &&
                parents(b, count, digests[cur].data());
            if (const auto bad = finish()) {
                return bad;
            }
            if (!read) {
                return -1;
            }
            hasher->Start(chunks[cur].data(), block, count, digests[cur].data());
            started = b;
            started_bytes = bytes;
            cur ^= 1;
        }
        return finish();
    }

private:
    void Advance(u64 bytes) {
        done += bytes;
        const u64 now = sceKernelGetProcessTimeWide();
        if (now - shown_us >= 100'000) {
            shown_us = now;
            progress(std::min(1.0f, static_cast<float>(done) / static_cast<float>(total)));
        }
    }

    FileUtil::IOFileBase& file;
    const u64 size;
    const std::function<void(float)>& progress;
    const bool quick;
    std::vector<u8> buffer;
    std::optional<BlockHasher> hasher;
    u64 total = 1;
    u64 done = 0;
    u64 shown_us = 0;
};

Result Damaged(const char* part, std::string detail) {
    return {Outcome::Damaged, part, std::move(detail)};
}

Result CutShort(const char* part, std::string detail) {
    return {Outcome::CutShort, part, std::move(detail)};
}

/// A RomFS's hash tree (IVFC) at `romfs`: three levels of SHA-256 digests of the level below's
/// blocks, the first under the header's master hashes, the third the data. On disc the header and
/// the master hashes come first, then level 3 aligned to its block size, then levels 1 and 2.
std::optional<Result> CheckIvfc(Checker& c, u64 romfs, const char* part) {
    std::array<u8, 0x60> header{};
    if (!c.Read(romfs, header.size(), header.data())) {
        return CutShort(part, "the RomFS header");
    }
    if (std::memcmp(header.data(), "IVFC", 4) != 0) {
        return Damaged(part, "no IVFC header");
    }
    u32 master_size;
    std::memcpy(&master_size, header.data() + 0x08, 4);
    struct Level {
        u64 size;
        u64 block;
        u64 offset;
    };
    std::array<Level, 3> levels{};
    for (std::size_t i = 0; i < 3; i++) {
        u32 log2;
        std::memcpy(&levels[i].size, header.data() + 0x14 + i * 0x18, 8);
        std::memcpy(&log2, header.data() + 0x1C + i * 0x18, 4);
        if (log2 < 6 || log2 > 24) {
            return Damaged(part, fmt::format("RomFS level {} block size 2^{}", i + 1, log2));
        }
        levels[i].block = u64{1} << log2;
    }
    const auto align = [](u64 x, u64 a) { return (x + a - 1) / a * a; };
    levels[2].offset = align(0x60 + master_size, levels[2].block);
    levels[0].offset = levels[2].offset + align(levels[2].size, levels[2].block);
    levels[1].offset = levels[0].offset + align(levels[0].size, levels[0].block);
    std::vector<u8> master(master_size);
    if (!c.Read(romfs + 0x60, master.size(), master.data())) {
        return CutShort(part, "the RomFS master hashes");
    }
    for (std::size_t i = 0; i < (c.Quick() ? 1 : 3); i++) {
        const auto parents = [&](u64 first, u64 count, u8* out) {
            if (i == 0) {
                if ((first + count) * 32 > master.size()) {
                    return false;
                }
                std::memcpy(out, master.data() + first * 32, static_cast<std::size_t>(count * 32));
                return true;
            }
            return c.Read(romfs + levels[i - 1].offset + first * 32,
                          static_cast<std::size_t>(count * 32), out);
        };
        const Level& level = levels[i];
        if (const auto bad = c.Level(romfs + level.offset, level.size, level.block, parents)) {
            return *bad < 0 ? CutShort(part, fmt::format("RomFS level {}", i + 1))
                            : Damaged(part, fmt::format("RomFS level {} block {}", i + 1, *bad));
        }
    }
    return std::nullopt;
}

/// An NCCH at `base`: none where it is whole, else what is wrong. `encrypted` is set, and nothing
/// checked, where it is.
std::optional<Result> CheckNcch(Checker& c, u64 base, const char* part, bool& encrypted) {
    NCCH_Header h{};
    if (!c.Read(base, sizeof(h), reinterpret_cast<u8*>(&h))) {
        return CutShort(part, "the NCCH header");
    }
    if (std::memcmp(&h.magic, "NCCH", 4) != 0) {
        return Damaged(part, "no NCCH header");
    }
    const u64 unit = MediaUnit << h.content_unit_size;
    if (base + u64{h.content_size} * unit > c.Size()) {
        return CutShort(part, fmt::format("{:#x} bytes from {:#x}, the file {:#x}",
                                          u64{h.content_size} * unit, base, c.Size()));
    }
    encrypted = !h.no_crypto;
    if (encrypted) {
        return std::nullopt;
    }
    const auto compare = [&](u64 offset, u64 length, const u8* want,
                             const char* what) -> std::optional<Result> {
        const std::optional<Digest> got = c.Hash(offset, length);
        if (!got) {
            return CutShort(part, what);
        }
        if (std::memcmp(got->data(), want, 32) != 0) {
            return Damaged(part, what);
        }
        return std::nullopt;
    };
    if (h.extended_header_size != 0) {
        if (auto bad = compare(base + 0x200, 0x400, h.extended_header_hash, "the extended header")) {
            return bad;
        }
    }
    if (h.logo_region_size != 0) {
        if (auto bad = compare(base + u64{h.logo_region_offset} * unit,
                               u64{h.logo_region_size} * unit, h.logo_region_hash, "the logo")) {
            return bad;
        }
    }
    if (h.exefs_size != 0) {
        const u64 exefs = base + u64{h.exefs_offset} * unit;
        if (auto bad = compare(exefs, u64{h.exefs_hash_region_size} * unit,
                               h.exefs_super_block_hash, "the ExeFS header")) {
            return bad;
        }
        // Ten files' names, offsets and sizes, then their digests from the last file's down.
        std::array<u8, 0x200> files{};
        if (!c.Read(exefs, files.size(), files.data())) {
            return CutShort(part, "the ExeFS header");
        }
        for (std::size_t i = 0; i < 10; i++) {
            u32 offset, length;
            std::memcpy(&offset, files.data() + i * 16 + 8, 4);
            std::memcpy(&length, files.data() + i * 16 + 12, 4);
            if (length == 0) {
                continue;
            }
            const std::string what =
                fmt::format("ExeFS {}", std::string(reinterpret_cast<const char*>(files.data() +
                                                                                  i * 16),
                                                    strnlen(reinterpret_cast<const char*>(
                                                                files.data() + i * 16),
                                                            8)));
            if (auto bad = compare(exefs + 0x200 + offset, length,
                                   files.data() + 0xC0 + (9 - i) * 0x20, what.c_str())) {
                return bad;
            }
        }
    }
    if (h.romfs_size != 0) {
        const u64 romfs = base + u64{h.romfs_offset} * unit;
        if (auto bad = compare(romfs, u64{h.romfs_hash_region_size} * unit,
                               h.romfs_super_block_hash, "the RomFS header")) {
            return bad;
        }
        if (auto bad = CheckIvfc(c, romfs, part)) {
            return bad;
        }
    }
    return std::nullopt;
}

Result CheckNcsd(Checker& c) {
    NCSD_Header h{};
    if (!c.Read(0, sizeof(h), reinterpret_cast<u8*>(&h))) {
        return CutShort(Translatable("the game"), "the NCSD header");
    }
    u64 total = 0;
    for (const auto& p : h.partitions) {
        total += u64{p.size} * MediaUnit;
    }
    c.SetTotal(total);
    for (u32 i = 0; i < 8; i++) {
        const auto& p = h.partitions[i];
        if (p.size == 0) {
            continue;
        }
        if (u64{p.offset + p.size} * MediaUnit > c.Size()) {
            return CutShort(PartName(i),
                            fmt::format("partition {} ends at {:#x}, the file at {:#x}", i,
                                        u64{p.offset + p.size} * MediaUnit, c.Size()));
        }
        // Quick: the game's partition alone beyond its place in the file.
        if (c.Quick() && i != 0) {
            continue;
        }
        bool encrypted = false;
        if (auto bad = CheckNcch(c, u64{p.offset} * MediaUnit, PartName(i), encrypted)) {
            return *bad;
        }
        // The game's own partition encrypted is an encrypted dump; another one left encrypted
        // (an update partition some tools skip) is not what the emulator reads.
        if (encrypted && i == 0) {
            return {Outcome::Encrypted};
        }
    }
    return {Outcome::Intact};
}

Result CheckCia(Checker& c, FileUtil::IOFileBase& file) {
    FileSys::CIAContainer cia;
    if (cia.Load(&file) != Loader::ResultStatus::Success) {
        return {Outcome::Unreadable, nullptr, "not a CIA"};
    }
    const FileSys::TitleMetadata& tmd = cia.GetTitleMetadata();
    u64 total = 0;
    for (std::size_t i = 0; i < tmd.GetContentCount(); i++) {
        total += cia.GetContentSize(i);
    }
    c.SetTotal(total);
    for (std::size_t i = 0; i < tmd.GetContentCount(); i++) {
        const u64 length = cia.GetContentSize(i);
        if (length == 0) {
            continue; // left out of this CIA
        }
        const char* const part = PartName(tmd.GetContentIndexByIndex(i));
        if (cia.GetContentOffset(i) + length > c.Size()) {
            return CutShort(part, fmt::format("content {}", i));
        }
        if (c.Quick()) {
            continue;
        }
        if (tmd.GetContentTypeByIndex(i) & 1) {
            return {Outcome::Encrypted};
        }
        const std::optional<Digest> got = c.Hash(cia.GetContentOffset(i), length);
        if (!got) {
            return CutShort(part, fmt::format("content {}", i));
        }
        if (std::memcmp(got->data(), tmd.GetContentHashByIndex(i).data(), 32) != 0) {
            return Damaged(part, fmt::format("content {}", i));
        }
    }
    return {Outcome::Intact};
}

bool HasExtension(const std::string& path, std::initializer_list<const char*> extensions) {
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const char* e) { return extension == e; });
}

Result Run(const std::string& path, const std::function<void(float)>& progress, bool quick) {
    const u64 start = sceKernelGetProcessTimeWide();
    Result result{Outcome::Unreadable};
    if (HasExtension(path, {".3dsx", ".z3dsx", ".elf", ".axf"})) {
        result = {Outcome::NoChecksums};
    } else if (HasExtension(path, {".cia", ".zcia"})) {
        std::unique_ptr<FileUtil::IOFileBase> file =
            std::make_unique<FileUtil::IOFile>(path, "rb");
        if (FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get())) {
            file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
        }
        if (file->IsOpen()) {
            Checker c{*file, progress, quick};
            result = CheckCia(c, *file);
        }
    } else {
        // NCSD or NCCH, through Z3DS and the console-unique crypto wrapper as the loader opens it.
        const std::unique_ptr<FileUtil::IOFileBase> file =
            FileSys::NCCHContainer::AutoOpenNCCHNCSD(path);
        u32 magic = 0;
        if (file && file->IsOpen() && file->ReadAtBytes(&magic, 4, 0x100) == 4) {
            Checker c{*file, progress, quick};
            if (magic == FileUtil::MakeMagic('N', 'C', 'S', 'D')) {
                result = CheckNcsd(c);
            } else if (magic == FileUtil::MakeMagic('N', 'C', 'C', 'H')) {
                NCCH_Header h{};
                c.Read(0, sizeof(h), reinterpret_cast<u8*>(&h));
                c.SetTotal(u64{h.content_size} * (MediaUnit << h.content_unit_size));
                bool encrypted = false;
                const std::optional<Result> bad =
                    CheckNcch(c, 0, Translatable("the game"), encrypted);
                result = bad ? *bad : encrypted ? Result{Outcome::Encrypted} : Result{Outcome::Intact};
            }
        }
    }
    LOG_INFO(Frontend, "{} of {}: outcome {}{}{} in {} ms", quick ? "Quick check" : "File check",
             path, static_cast<int>(result.outcome), result.part ? ", " : "",
             result.part ? result.part : "", (sceKernelGetProcessTimeWide() - start) / 1000);
    if (!result.detail.empty()) {
        LOG_INFO(Frontend, "File check of {}: {}", path, result.detail);
    }
    return result;
}

} // Anonymous namespace

Result Check(const std::string& path, const std::function<void(float)>& progress) {
    return Run(path, progress, false);
}

Result QuickCheck(const std::string& path) {
    return Run(path, [](float) {}, true);
}

} // namespace VitaFrontend::RomCheck
