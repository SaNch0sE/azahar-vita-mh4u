// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <vector>
#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include "common/archives.h"
#include "common/logging/log.h"
#include "common/mutex.h"
#include "common/process_clock.h"
#ifdef VITA_DIAGNOSTICS
#include <atomic>
#endif
#include "core/file_sys/archive_artic.h"
#include "core/file_sys/archive_backend.h"
#include "core/file_sys/romfs_reader.h"
#include "core/hle/service/fs/fs_user.h"
#include "core/loader/loader.h"

SERIALIZE_EXPORT_IMPL(FileSys::DirectRomFSReader)

namespace FileSys {

namespace {
u64 NowUs() {
    return static_cast<u64>(Common::ProcessClock::now().time_since_epoch().count());
}
} // namespace

#ifdef VITA_DIAGNOSTICS
namespace {
struct {
    std::atomic<u64> reads, read_bytes, misses, miss_us, large, large_bytes, large_us;
} read_stats;
} // namespace

RomFSReadStats TakeRomFSReadStats() {
    const auto take = [](std::atomic<u64>& v) { return v.exchange(0, std::memory_order_relaxed); };
    return {take(read_stats.reads),  take(read_stats.read_bytes),  take(read_stats.misses),
            take(read_stats.miss_us), take(read_stats.large),       take(read_stats.large_bytes),
            take(read_stats.large_us)};
}
#endif

std::size_t DirectRomFSReader::ReadFile(std::size_t offset, std::size_t length, u8* buffer) {
    length = std::min(length, GetSize() - offset);
    if (length == 0)
        return 0; // Crypto++ does not like zero size buffer

    const auto segments = BreakupRead(offset, length);
    std::size_t read_progress = 0;

    // Skip cache if the read is too big
    if (segments.size() == 1 && segments[0].second > cache_line_size()) {
        const u64 start = NowUs();
        length = file->ReadAtBytes(buffer, length, offset);
        const u64 took = NowUs() - start;
#ifdef VITA_DIAGNOSTICS
        read_stats.large.fetch_add(1, std::memory_order_relaxed);
        read_stats.large_bytes.fetch_add(length, std::memory_order_relaxed);
        read_stats.large_us.fetch_add(took, std::memory_order_relaxed);
#endif
        LOG_TRACE(Service_FS, "RomFS Cache SKIP: offset={}, length={}, {} us", offset, length,
                  took);
        return length;
    }

    // TODO(PabloMK7): Make cache thread safe, read the comment in CacheReady function.
    // std::unique_lock<std::shared_mutex> read_guard(cache_mutex);
    for (const auto& seg : segments) {
        std::size_t read_size = cache_line_size();
        std::size_t page = OffsetToPage(seg.first);
        // Check if segment is in cache
        auto cache_entry = cache.Request(page);
        if (!cache_entry.first) {
            // If not found, read from disk and cache the data
            const u64 start = NowUs();
            read_size = file->ReadAtBytes(cache_entry.second, read_size, page);
            const u64 took = NowUs() - start;
#ifdef VITA_DIAGNOSTICS
            read_stats.misses.fetch_add(1, std::memory_order_relaxed);
            read_stats.miss_us.fetch_add(took, std::memory_order_relaxed);
#endif
            LOG_TRACE(Service_FS, "RomFS Cache MISS: page={}, length={}, into={}, {} us", page,
                      seg.second, (seg.first - page), took);
        } else {
            LOG_TRACE(Service_FS, "RomFS Cache HIT: page={}, length={}, into={}", page, seg.second,
                      (seg.first - page));
        }
        std::size_t copy_amount =
            (read_size > (seg.first - page))
                ? std::min((seg.first - page) + seg.second, read_size) - (seg.first - page)
                : 0;
        std::memcpy(buffer + read_progress, cache_entry.second + (seg.first - page), copy_amount);
        read_progress += copy_amount;
    }
#ifdef VITA_DIAGNOSTICS
    read_stats.reads.fetch_add(1, std::memory_order_relaxed);
    read_stats.read_bytes.fetch_add(read_progress, std::memory_order_relaxed);
#endif
    return read_progress;
}

DirectRomFSReader::LineCache::LineCache(const CacheSpec& spec)
    : line_size{spec.line_size}, line_count{spec.line_count},
      storage{spec.line_size * spec.line_count, spec.name, spec.placement},
      line_page(spec.line_count, static_cast<std::size_t>(-1)), line_used(spec.line_count, 0) {}

std::pair<bool, u8*> DirectRomFSReader::LineCache::Request(std::size_t page) {
    ++clock;
    if (const auto it = index.find(page); it != index.end()) {
        line_used[it->second] = clock;
        return {true, storage.Data() + it->second * line_size};
    }
    std::size_t victim = 0;
    for (std::size_t i = 1; i < line_count; i++) {
        if (line_used[i] < line_used[victim]) {
            victim = i;
        }
    }
    if (line_page[victim] != static_cast<std::size_t>(-1)) {
        index.erase(line_page[victim]);
    }
    line_page[victim] = page;
    line_used[victim] = clock;
    index.emplace(page, victim);
    return {false, storage.Data() + victim * line_size};
}

DirectRomFSReader::CacheSpec DirectRomFSReader::PickCacheSpec() {
#ifdef __vita__
    // The first reader is the title's RomFS. One 16 MiB cache per process: a second of that
    // size would not fit the pool.
    static bool big_cache_taken = false;
    if (!big_cache_taken) {
        big_cache_taken = true;
        return {64 * 1024, 256, "azahar-romfs-cache",
                Common::HostSharedMemory::Placement::PhysicallyContiguous};
    }
#endif
    return {8 * 1024, 16, "azahar-romfs-cache-small"};
}

bool DirectRomFSReader::AllowsCachedReads() const {
    return true;
}

bool DirectRomFSReader::CacheReady(std::size_t file_offset, std::size_t length) {
    auto segments = BreakupRead(file_offset, length);
    if (segments.size() == 1 && segments[0].second > cache_line_size()) {
        return false;
    } else {
        // TODO(PabloMK7): Since the LRU cache is not thread safe, a lock must be used.
        // However, this completely breaks the point of using a cache, because
        // smaller reads may be blocked by bigger reads. For now, always return
        // data being in cache to prevent the need of a lock, and only read data
        // asynchronously if it is too big to use the cache.
        /*
        std::shared_lock<Common::SharedMutex> read_guard(cache_mutex);
        for (auto it = segments.begin(); it != segments.end(); it++) {
            if (!cache.contains(OffsetToPage(it->first)))
                return false;
        }
        */
        return true;
    }
}

std::vector<std::pair<std::size_t, std::size_t>> DirectRomFSReader::BreakupRead(
    std::size_t offset, std::size_t length) {

    std::vector<std::pair<std::size_t, std::size_t>> ret;

    // Reads bigger than the cache line size will probably never hit again
    if (length > cache_line_size()) {
        ret.push_back(std::make_pair(offset, length));
        return ret;
    }

    std::size_t curr_offset = offset;
    while (length) {
        std::size_t next_page = OffsetToPage(curr_offset + cache_line_size());
        std::size_t curr_page_len = std::min(length, next_page - curr_offset);
        ret.push_back(std::make_pair(curr_offset, curr_page_len));
        curr_offset = next_page;
        length -= curr_page_len;
    }
    return ret;
}

ArticRomFSReader::ArticRomFSReader(std::shared_ptr<Network::ArticBase::Client>& cli,
                                   bool is_update_romfs)
    : client(cli), cache(cli) {
    auto req = client->NewRequest("FSUSER_OpenFileDirectly");

    FileSys::Path archive(FileSys::LowPathType::Empty, {});
    std::vector<u8> fileVec(0xC);
    fileVec[0] = static_cast<u8>(is_update_romfs ? 5 : 0);
    FileSys::Path file(FileSys::LowPathType::Binary, fileVec);

    req.AddParameterS32(static_cast<s32>(Service::FS::ArchiveIdCode::SelfNCCH));

    auto archive_buf = ArticArchive::BuildFSPath(archive);
    req.AddParameterBuffer(archive_buf.data(), archive_buf.size());
    auto file_buf = ArticArchive::BuildFSPath(file);
    req.AddParameterBuffer(file_buf.data(), file_buf.size());

    req.AddParameterS32(1);
    req.AddParameterS32(0);

    auto resp = client->Send(req);

    if (!resp.has_value() || !resp->Succeeded()) {
        load_status = Loader::ResultStatus::Error;
        return;
    }
    if (resp->GetMethodResult() != 0) {
        load_status = Loader::ResultStatus::ErrorNotUsed;
        return;
    }

    auto handle_buf = resp->GetResponseBuffer(0);
    if (!handle_buf.has_value() || handle_buf->second != sizeof(s32)) {
        load_status = Loader::ResultStatus::Error;
        return;
    }

    romfs_handle = *reinterpret_cast<s32*>(handle_buf->first);

    req = client->NewRequest("FSFILE_GetSize");

    req.AddParameterS32(romfs_handle);

    resp = client->Send(req);

    if (!resp.has_value() || !resp->Succeeded()) {
        load_status = Loader::ResultStatus::Error;
        return;
    }
    if (resp->GetMethodResult() != 0) {
        load_status = Loader::ResultStatus::ErrorNotUsed;
        return;
    }

    auto size_buf = resp->GetResponseBuffer(0);
    if (!size_buf.has_value() || size_buf->second != sizeof(u64)) {
        load_status = Loader::ResultStatus::Error;
        return;
    }

    data_size = static_cast<size_t>(*reinterpret_cast<u64*>(size_buf->first));
    load_status = Loader::ResultStatus::Success;
}

ArticRomFSReader::~ArticRomFSReader() {
    if (romfs_handle != -1) {
        auto req = client->NewRequest("FSFILE_Close");
        req.AddParameterS32(romfs_handle);
        client->Send(req);
        romfs_handle = -1;
    }
}

std::size_t ArticRomFSReader::ReadFile(std::size_t offset, std::size_t length, u8* buffer) {
    length = std::min(length, static_cast<std::size_t>(data_size) - offset);
    auto res = cache.Read(romfs_handle, offset, length, buffer);
    if (res.Failed())
        return 0;
    return res.Unwrap();
}

bool ArticRomFSReader::AllowsCachedReads() const {
    return true;
}

bool ArticRomFSReader::CacheReady(std::size_t file_offset, std::size_t length) {
    return cache.CacheReady(file_offset, length);
}

void ArticRomFSReader::CloseFile() {
    if (romfs_handle != -1) {
        auto req = client->NewRequest("FSFILE_Close");
        req.AddParameterS32(romfs_handle);
        client->Send(req);
        romfs_handle = -1;
    }
}

} // namespace FileSys
