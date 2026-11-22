// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <fmt/format.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include "common/assert.h"
#include "common/build_id.h"
#include "common/file_util.h"
#include "common/hash.h"
#include "common/named_thread.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_shader_disk_cache.h"

namespace SgxdRenderer {

namespace {

// A record: its byte count, the key, vertex (0 or 1), the input registers, the program's bytes
// (Usse::SerializeProgram), then a checksum of everything before it. The file is its records, one
// after the other.
constexpr std::size_t RecordHeadBytes = 20;
constexpr std::size_t RecordTailBytes = 4;
constexpr std::size_t MaxRecordBytes = 1u << 20;
constexpr std::size_t ReadBytes = 64 * 1024;
/// A batch is written once it is this large or its oldest program has waited this long.
constexpr std::size_t BatchBytes = 256 * 1024;
constexpr u32 BatchUs = 10'000'000;
constexpr const char* Directory = ShaderDiskCache::Root;
#ifdef VITA_DIAGNOSTICS
constexpr const char* BuildKind = "diag";
#else
constexpr const char* BuildKind = "rel";
#endif

u32 Checksum(const u8* data, std::size_t size) {
    u32 hash = 2166136261u;
    for (std::size_t i = 0; i < size; i++) {
        hash = (hash ^ data[i]) * 16777619u;
    }
    return hash;
}

template <typename T>
void Put(std::vector<u8>& out, T value) {
    const auto* p = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

template <typename T>
T Get(const u8* p) {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

using Entry = ShaderDiskCache::Entry;

/// The open version's file and the programs read from it, kept for the process.
struct Shared {
    std::mutex mutex;
    std::string path;                         ///< written by Open before its loader starts, Forget
    std::unordered_map<u64, Entry> loaded[2]; ///< the programs read or stored, [vertex]
    std::vector<u8> pending;                  ///< the records stored and not written yet
    u64 pending_since = 0;                    ///< process time of the oldest one's Store
    u64 file_bytes = 0;                       ///< the file's whole records
    bool read = false;                        ///< the loader is through the file
    bool writing = false;                     ///< a worker is writing a batch
    bool failed = false;                      ///< a write failed: none after it this run
    Common::NamedThread loader;               ///< last: joined before the rest goes
};

Shared& State() {
    static Shared shared;
    return shared;
}

/// The whole records at the front of `bytes`, each passed to `record`. Returns their bytes;
/// `stop` is set at a record that is none (its byte count, or `record` refusing it).
template <typename F>
std::size_t Records(const u8* bytes, std::size_t size, bool& stop, F&& record) {
    std::size_t at = 0;
    while (size - at >= 4) {
        const u32 length = Get<u32>(bytes + at);
        if (length < RecordHeadBytes + RecordTailBytes || length > MaxRecordBytes) {
            stop = true;
            break;
        }
        if (size - at < length) {
            break;
        }
        if (!record(bytes + at, length)) {
            stop = true;
            break;
        }
        at += length;
    }
    return at;
}

/// The loader: the file's programs read into memory. A file whose records end before it does (a
/// stop cut its last write short, or the card damaged one: its checksum) is cut back to them.
void Load(std::stop_token stop) {
    Common::SetCurrentThreadRole(Common::ThreadRole::ShaderCompiler);
    Shared& s = State();
    const SceInt64 start = sceKernelGetProcessTimeWide();
    u64 whole = 0, size = 0;
    bool removed = false;
    // A read that failed is no end of the file: the file stays as it is, and this run stores nothing
    // (where its records end is not known).
    bool unread = false;
    unsigned read[2] = {};
    std::unordered_set<u64>
        stored_first[2]; // in the file and in `pending`: a Store before its read
    // Common's file opens again where it was after a standby closed its descriptor.
    if (FileUtil::IOFile file{s.path, "rb"}; file.IsOpen()) {
        std::vector<u8> bytes;
        std::size_t have = 0;
        bool end = false;
        while (!end) {
            if (stop.stop_requested()) {
                return;
            }
            bytes.resize(have + ReadBytes);
            const std::size_t got = file.ReadBytes(bytes.data() + have, ReadBytes);
            if (got == 0) {
                unread = !file.IsGood();
                break;
            }
            have += got;
            size += got;
            const std::size_t used =
                Records(bytes.data(), have, end, [&](const u8* r, std::size_t length) {
                    const u64 key = Get<u64>(r + 4);
                    const u32 vertex = Get<u32>(r + 12);
                    const std::size_t body = length - RecordTailBytes;
                    if (vertex > 1 || Checksum(r, body) != Get<u32>(r + body)) {
                        return false;
                    }
                    Entry entry{};
                    entry.input_regs = Get<u32>(r + 16);
                    // The version covers the serialiser: a record that passed its checksum and
                    // does not read back is this emitter's own bug.
                    const bool parsed = Usse::DeserializeProgram(
                        r + RecordHeadBytes, body - RecordHeadBytes, entry.program);
                    ASSERT_MSG(parsed, "sgxd: shader cache record {:016x} does not read back", key);
                    std::scoped_lock lock{s.mutex};
                    if (s.loaded[vertex].try_emplace(key, std::move(entry)).second) {
                        read[vertex]++;
                    } else {
                        stored_first[vertex].insert(key);
                    }
                    return true;
                });
            whole += used;
            std::memmove(bytes.data(), bytes.data() + used, have - used);
            have -= used;
        }
        if (end) {
            // No size (0) is a failed read too.
            size = file.GetSize();
            unread = size < whole;
        }
    }
    if (!unread && whole != size) {
        SceIoStat stat{};
        stat.st_size = static_cast<SceOff>(whole);
        // A file that cannot be cut goes, and this run stores nothing: its batches would
        // follow the cut record.
        if (sceIoChstat(s.path.c_str(), &stat, SCE_CST_SIZE) < 0) {
            sceIoRemove(s.path.c_str());
            removed = true;
        }
    }
    sceClibPrintf("[azahar] shader cache: %u programs read in %u ms, %u damaged\n", read,
                  static_cast<unsigned>((sceKernelGetProcessTimeWide() - start) / 1000), damaged);
}

} // namespace

void ShaderDiskCache::Open(u64 title_id) {
    if (opened) {
        return;
    }
    opened = true;
    if (!Settings::values.use_disk_shader_cache.GetValue() || title_id == 0) {
        return;
    }
    const std::span<const u8> build = Common::BuildId();
    ASSERT_MSG(!build.empty(), "sgxd: the shader cache's version is the build ID, and this "
                               "executable was linked without one");
    const std::string flags = SgxdFlagsText();
    const u64 version = Common::ComputeHash64(build.data(), build.size()) ^
                        Common::ComputeHash64(flags.data(), flags.size());
    const std::string directory =
        fmt::format("{}/{:016x}.{}.{:016x}", Directory, title_id, BuildKind, version);
    const std::string path = directory + "/programs";
    Shared& s = State();
    // The process's first title, or another one: a savestate load's new rasterizer keeps what
    // its title's loader read.
    if (s.path != path) {
        s.loader = Common::NamedThread{};
        std::scoped_lock lock{s.mutex};
        ASSERT_MSG(!s.writing, "sgxd: a shader cache write is under way at Open");
        s.path = path;
        for (int v = 0; v < 2; v++) {
            s.loaded[v].clear();
        }
        s.pending.clear();
        s.file_bytes = 0;
        s.read = false;
        s.failed = false;
        sceIoMkdir(Directory, 0777);
        sceIoMkdir(path.c_str(), 0777);
        // The programs there (PathIn's names).
        if (const SceUID d = sceIoDopen(path.c_str()); d >= 0) {
            SceIoDirent entry{};
            while (sceIoDread(d, &entry) > 0) {
                const bool vertex = EndsWith(entry.d_name, ".v");
                char* end = nullptr;
                const u64 key = std::strtoull(entry.d_name, &end, 16);
                if ((vertex || EndsWith(entry.d_name, ".f")) && end == entry.d_name + 16 &&
                    *end == '.') {
                    s.keys[vertex ? 1 : 0].insert(key);
                }
            }
            sceIoDclose(d);
        }
        sceClibPrintf("[azahar] shader cache %s: %u fragment and %u vertex programs\n",
                      path.c_str(), static_cast<unsigned>(s.keys[0].size()),
                      static_cast<unsigned>(s.keys[1].size()));
        s.loader = Common::NamedThread{Common::ThreadCfg{"SgxdShaderLoad", 256 * 1024},
                                       [](std::stop_token stop) { Load(stop); }};
    }
    ready.store(true, std::memory_order_release);
}

void ShaderDiskCache::Forget(u64 title_id) {
    Shared& s = State();
    if (!s.path.starts_with(fmt::format("{}/{:016x}.", Directory, title_id))) {
        return;
    }
    s.loader = Common::NamedThread{};
    std::scoped_lock lock{s.mutex};
    s.path.clear();
    for (int v = 0; v < 2; v++) {
        s.loaded[v].clear();
    }
    s.pending.clear();
    s.file_bytes = 0;
    s.read = false;
}

std::optional<ShaderDiskCache::Entry> ShaderDiskCache::Find(u64 key, bool vertex) const {
    if (!ready.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    Shared& s = State();
    std::scoped_lock lock{s.mutex};
    const auto found = s.loaded[vertex ? 1 : 0].find(key);
    if (found == s.loaded[vertex ? 1 : 0].end()) {
        return std::nullopt;
    }
    return found->second;
}

bool ShaderDiskCache::Contains(u64 key, bool vertex) const {
    if (!ready.load(std::memory_order_acquire)) {
        return false;
    }
    Shared& s = State();
    std::scoped_lock lock{s.mutex};
    return s.loaded[vertex ? 1 : 0].contains(key);
}

void ShaderDiskCache::Store(u64 key, bool vertex, const Usse::Program& program, u32 input_regs) {
    if (!ready.load(std::memory_order_acquire)) {
        return;
    }
    Shared& s = State();
    const int v = vertex ? 1 : 0;
    {
        std::scoped_lock lock{s.mutex};
        if (s.loaded[v].contains(key)) {
            return;
        }
    }
    const std::vector<u8> bytes = Usse::SerializeProgram(program);
    // What a later run reads back must be this program, field by field: a field the serialiser
    // leaves out comes back as its default, and the runs after draw with another program. The
    // peak fields are dumpusse's, not kept.
    Usse::Program back;
    const bool read = Usse::DeserializeProgram(bytes.data(), bytes.size(), back);
    back.peak_at = program.peak_at;
    back.peak_regs = program.peak_regs;
    ASSERT_MSG(read && back == program,
               "sgxd: shader cache record {:016x} does not read back as the program it holds",
               key);
    const std::size_t length = RecordHeadBytes + bytes.size() + RecordTailBytes;
    ASSERT_MSG(length <= MaxRecordBytes, "sgxd: shader cache record {:016x} of {} bytes", key,
               length);
    std::vector<u8> record;
    record.reserve(length);
    Put(record, static_cast<u32>(length));
    Put(record, key);
    Put(record, vertex ? 1u : 0u);
    Put(record, input_regs);
    record.insert(record.end(), bytes.begin(), bytes.end());
    Put(record, Checksum(record.data(), record.size()));
    {
        std::scoped_lock lock{s.mutex};
        // The other tier worker's Store of this key came first.
        if (!s.loaded[v].try_emplace(key, Entry{program, input_regs}).second) {
            return;
        }
        if (s.pending.empty()) {
            s.pending_since = static_cast<u64>(sceKernelGetProcessTimeWide());
        }
        s.pending.insert(s.pending.end(), record.begin(), record.end());
    }
    WriteBatch();
}

u32 ShaderDiskCache::WriteBatch(bool all) {
    if (!ready.load(std::memory_order_acquire)) {
        return 0;
    }
    Shared& s = State();
    std::vector<u8> batch;
    std::string path;
    u64 at;
    {
        std::scoped_lock lock{s.mutex};
        if (s.failed) {
            s.pending.clear();
        }
        if (s.pending.empty() || s.writing) {
            return 0;
        }
        const u64 waited = static_cast<u64>(sceKernelGetProcessTimeWide()) - s.pending_since;
        if (!s.read) {
            return BatchUs;
        }
        if (!all && s.pending.size() < BatchBytes && waited < BatchUs) {
            return static_cast<u32>(BatchUs - waited);
        }
        batch = std::exchange(s.pending, {});
        path = s.path;
        at = s.file_bytes;
        s.writing = true;
    }
    // One open, one write and one close for the batch: every operation on the card is one the
    // title's reads wait behind. A standby closes the descriptor between them: the three once more,
    // the bytes to the same place.
    bool written = false;
    for (int attempt = 0; attempt < 2; attempt++) {
        const SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT, 0666);
        if (fd < 0) {
            break;
        }
        const int put = sceIoPwrite(fd, batch.data(), batch.size(), static_cast<SceOff>(at));
        sceIoClose(fd);
        written = put == static_cast<int>(batch.size());
        if (put != FileUtil::StandbyClosed) {
            break;
        }
    }
    std::scoped_lock lock{s.mutex};
    s.writing = false;
    if (written) {
        s.file_bytes += batch.size();
    } else {
        // A full card, as a rule. What reached the file is cut back by the next run's loader.
        s.failed = true;
        s.pending.clear();
    }
    return 0;
}

} // namespace SgxdRenderer
