// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/rtc.h>
#include <zlib.h>
#include "citra_vita/bug_report.h"
#include "common/common_types.h"
#include "common/scm_rev.h"
#include "common/vita_data_dir.h"
#include "core/arm/vita/arm_vita_native.h"

namespace VitaFrontend {

namespace {

// Two lines a second: two minutes, in a fixed ring (120 KiB, never allocated per line).
constexpr std::size_t KeptLines = 240;
constexpr std::size_t LineBytes = 512;
// Reports kept in ux0:data/mh4u/reports: a new one deletes the oldest past this.
constexpr std::size_t KeptReports = 3;

std::mutex lines_mutex;
char recent_lines[KeptLines][LineBytes];
std::size_t lines_written = 0;

struct Entry {
    std::string name;
    bool dir;
    u64 size;
    SceDateTime mtime;
};

std::vector<Entry> ListDir(const std::string& path) {
    std::vector<Entry> out;
    const SceUID d = sceIoDopen(path.c_str());
    if (d < 0) {
        return out;
    }
    SceIoDirent ent;
    while (std::memset(&ent, 0, sizeof(ent)), sceIoDread(d, &ent) > 0) {
        if (std::strcmp(ent.d_name, ".") == 0 || std::strcmp(ent.d_name, "..") == 0) {
            continue;
        }
        out.push_back({ent.d_name, SCE_S_ISDIR(ent.d_stat.st_mode) != 0,
                       static_cast<u64>(ent.d_stat.st_size), ent.d_stat.st_mtime});
    }
    sceIoDclose(d);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
}

u64 TimeKey(const SceDateTime& t) {
    return ((((static_cast<u64>(t.year) * 13 + t.month) * 32 + t.day) * 24 + t.hour) * 60 +
            t.minute) * 60 + t.second;
}

std::string FormatTime(const SceDateTime& t) {
    return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", t.year, t.month, t.day, t.hour,
                       t.minute, t.second);
}

bool ReadText(const std::string& path, std::string& out, std::size_t limit = 64 * 1024) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    out.assign(limit, '\0');
    out.resize(std::fread(out.data(), 1, limit, f));
    std::fclose(f);
    return true;
}

/// A zip written as it goes: each entry's header is patched once its data is in.
class ZipWriter {
public:
    explicit ZipWriter(const std::string& path) : file(std::fopen(path.c_str(), "wb")) {
        SceDateTime now{};
        sceRtcGetCurrentClockLocalTime(&now);
        dos_time = static_cast<u16>((now.hour << 11) | (now.minute << 5) | (now.second / 2));
        dos_date = static_cast<u16>(((now.year - 1980) << 9) | (now.month << 5) | now.day);
    }
    ~ZipWriter() {
        if (file != nullptr) {
            std::fclose(file);
        }
    }

    bool Ok() const {
        return file != nullptr && !failed;
    }

    void AddText(const std::string& name, const std::string& text) {
        std::size_t pos = 0;
        Add(name, true, [&](u8* buf, std::size_t cap) {
            const std::size_t n = std::min(cap, text.size() - pos);
            std::memcpy(buf, text.data() + pos, n);
            pos += n;
            return n;
        });
    }

    /// deflate off for data that is already compressed (save states).
    bool AddFile(const std::string& name, const std::string& path, bool deflate) {
        std::FILE* in = std::fopen(path.c_str(), "rb");
        if (in == nullptr) {
            return false;
        }
        Add(name, deflate, [&](u8* buf, std::size_t cap) { return std::fread(buf, 1, cap, in); });
        std::fclose(in);
        return true;
    }

    /// Everything under a folder, its layout kept under prefix.
    void AddTree(const std::string& prefix, const std::string& path) {
        for (const Entry& e : ListDir(path)) {
            if (e.dir) {
                AddTree(prefix + e.name + "/", path + "/" + e.name);
            } else {
                AddFile(prefix + e.name, path + "/" + e.name, true);
            }
        }
    }

    bool Finish() {
        if (file == nullptr) {
            return false;
        }
        const long cd_start = std::ftell(file);
        for (const auto& c : central) {
            Put32(0x02014b50);
            Put16(20);
            Put16(20);
            Put16(0);
            Put16(c.method);
            Put16(dos_time);
            Put16(dos_date);
            Put32(c.crc);
            Put32(c.csize);
            Put32(c.usize);
            Put16(static_cast<u16>(c.name.size()));
            Put16(0);
            Put16(0);
            Put16(0);
            Put16(0);
            Put32(0);
            Put32(c.offset);
            std::fwrite(c.name.data(), 1, c.name.size(), file);
        }
        const long cd_end = std::ftell(file);
        Put32(0x06054b50);
        Put16(0);
        Put16(0);
        Put16(static_cast<u16>(central.size()));
        Put16(static_cast<u16>(central.size()));
        Put32(static_cast<u32>(cd_end - cd_start));
        Put32(static_cast<u32>(cd_start));
        Put16(0);
        const bool ok = !failed && std::ferror(file) == 0;
        failed |= std::fclose(file) != 0;
        file = nullptr;
        return ok && !failed;
    }

private:
    struct Central {
        std::string name;
        u16 method;
        u32 crc, csize, usize, offset;
    };

    void Put16(u16 v) {
        const u8 b[2] = {static_cast<u8>(v), static_cast<u8>(v >> 8)};
        failed |= std::fwrite(b, 1, 2, file) != 2;
    }
    void Put32(u32 v) {
        const u8 b[4] = {static_cast<u8>(v), static_cast<u8>(v >> 8), static_cast<u8>(v >> 16),
                         static_cast<u8>(v >> 24)};
        failed |= std::fwrite(b, 1, 4, file) != 4;
    }
    void LocalHeader(const Central& c) {
        Put32(0x04034b50);
        Put16(20);
        Put16(0);
        Put16(c.method);
        Put16(dos_time);
        Put16(dos_date);
        Put32(c.crc);
        Put32(c.csize);
        Put32(c.usize);
        Put16(static_cast<u16>(c.name.size()));
        Put16(0);
        std::fwrite(c.name.data(), 1, c.name.size(), file);
    }

    template <typename Read>
    void Add(const std::string& name, bool deflate, Read&& read) {
        if (!Ok()) {
            return;
        }
        Central c{name, static_cast<u16>(deflate ? 8 : 0), 0, 0, 0,
                  static_cast<u32>(std::ftell(file))};
        LocalHeader(c);
        static std::vector<u8> in(64 * 1024), out(64 * 1024);
        uLong crc = crc32(0, nullptr, 0);
        z_stream z{};
        if (deflate && deflateInit2(&z, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            failed = true;
            return;
        }
        for (;;) {
            const std::size_t n = read(in.data(), in.size());
            crc = crc32(crc, in.data(), static_cast<uInt>(n));
            c.usize += static_cast<u32>(n);
            if (!deflate) {
                failed |= std::fwrite(in.data(), 1, n, file) != n;
                c.csize += static_cast<u32>(n);
                if (n == 0) {
                    break;
                }
                continue;
            }
            z.next_in = in.data();
            z.avail_in = static_cast<uInt>(n);
            const int flush = n == 0 ? Z_FINISH : Z_NO_FLUSH;
            int r;
            do {
                z.next_out = out.data();
                z.avail_out = static_cast<uInt>(out.size());
                r = ::deflate(&z, flush);
                const std::size_t produced = out.size() - z.avail_out;
                failed |= std::fwrite(out.data(), 1, produced, file) != produced;
                c.csize += static_cast<u32>(produced);
            } while (z.avail_out == 0 || (flush == Z_FINISH && r != Z_STREAM_END));
            if (n == 0) {
                break;
            }
        }
        if (deflate) {
            deflateEnd(&z);
        }
        c.crc = static_cast<u32>(crc);
        const long end = std::ftell(file);
        std::fseek(file, static_cast<long>(c.offset), SEEK_SET);
        LocalHeader(c);
        std::fseek(file, end, SEEK_SET);
        central.push_back(std::move(c));
    }

    std::FILE* file;
    bool failed{};
    u16 dos_time{}, dos_date{};
    std::vector<Central> central;
};

const char* GameProgramName() {
    switch (Core::mh4u_code_version.load(std::memory_order_relaxed)) {
    case 11:
        return "v1.1 (the update's program)";
    case 10:
        return "v1.0 (the base game's program, as the web installer's mod brings)";
    case -1:
        return "not recognised (the port's game fixes are off)";
    default:
        return "unknown (not started yet)";
    }
}

void ListInto(std::string& out, const std::string& path, int depth) {
    for (const Entry& e : ListDir(path)) {
        out += fmt::format("{:{}}{}{}  {}  {}\n", "", depth * 2, e.name, e.dir ? "/" : "",
                           e.dir ? std::string{} : fmt::format("{} bytes", e.size),
                           FormatTime(e.mtime));
    }
}

} // namespace

void ReportPrintf(const char* format, ...) {
    std::lock_guard lock{lines_mutex};
    char* line = recent_lines[lines_written % KeptLines];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, LineBytes, format, args);
    va_end(args);
    lines_written++;
    sceClibPrintf("%s", line);
}

std::string WriteBugReport(bool with_state) {
    const std::string base = VITA_DATA_DIR;
    const std::string dir = base + "/reports";
    sceIoMkdir(dir.c_str(), 0777);
    SceDateTime now{};
    sceRtcGetCurrentClockLocalTime(&now);
    const std::string file_name =
        fmt::format("mh4u_report_{:04}{:02}{:02}_{:02}{:02}{:02}.zip", now.year, now.month,
                    now.day, now.hour, now.minute, now.second);
    const std::string path = dir + "/" + file_name;
    // Keep the card from filling: older reports go, the newest KeptReports - 1 stay beside the
    // new one (names sort by date).
    {
        std::vector<std::string> old;
        for (const Entry& e : ListDir(dir)) {
            if (!e.dir && e.name.rfind("mh4u_report_", 0) == 0) {
                old.push_back(e.name);
            }
        }
        for (std::size_t i = 0; i + (KeptReports - 1) < old.size(); i++) {
            sceIoRemove((dir + "/" + old[i]).c_str());
        }
    }

    // Newest crash dump (by the time in its name) and newest save state (by date).
    std::string dump;
    u64 dump_time = 0;
    for (const Entry& e : ListDir("ux0:data")) {
        const bool is_dump = e.name.rfind("psp2core-", 0) == 0 &&
                             e.name.size() > 8 &&
                             e.name.compare(e.name.size() - 8, 8, ".psp2dmp") == 0;
        if (is_dump) {
            const u64 t = std::strtoull(e.name.c_str() + 9, nullptr, 10);
            if (t >= dump_time) {
                dump_time = t;
                dump = e.name;
            }
        }
    }
    std::string state;
    u64 state_time = 0;
    for (const Entry& e : ListDir(base + "/states")) {
        if (!e.dir && e.name.size() > 4 && e.name.compare(e.name.size() - 4, 4, ".cst") == 0 &&
            TimeKey(e.mtime) >= state_time) {
            state_time = TimeKey(e.mtime);
            state = e.name;
        }
    }

    std::string report = "MH4U for PS Vita - bug report\n\n";
    report += fmt::format("App: {} {} ({}), built {}\n", Common::g_build_name,
                          Common::g_scm_desc, Common::g_scm_rev, Common::g_build_date);
    report += fmt::format("Written: {}\n", FormatTime(now));
    report += fmt::format("Game program: {}\n", GameProgramName());
    report += fmt::format("Save state included: {}\n", with_state && !state.empty() ? state : "no");
    report += fmt::format("Crash dump included: {}\n\n", dump.empty() ? "none on the card" : dump);

    const std::string mod = base + "/load/mods/0004000000126300";
    report += "== Mod folder (load/mods/0004000000126300)\n";
    ListInto(report, mod, 0);
    report += "  romfs:\n";
    ListInto(report, mod + "/romfs", 2);
    report += "  exefs:\n";
    ListInto(report, mod + "/exefs", 2);
    report += "\n== ux0:data/mh4u\n";
    ListInto(report, base, 0);
    report += "\n== Save states\n";
    ListInto(report, base + "/states", 0);
    report += "\n== Crash dumps in ux0:data\n";
    for (const Entry& e : ListDir("ux0:data")) {
        if (e.name.rfind("psp2core-", 0) == 0) {
            report += fmt::format("{}  {} bytes\n", e.name, e.size);
        }
    }
    for (const char* name : {"video.txt", "gxm_flags.txt", "cheats/0004000000126300.txt"}) {
        std::string text;
        if (ReadText(base + "/" + name, text)) {
            report += fmt::format("\n== {}\n{}\n", name, text);
        }
    }
    report += "\n== The last two minutes, a line a second (speed, then CPU time per thread)\n";
    {
        std::lock_guard lock{lines_mutex};
        const std::size_t count = std::min(lines_written, KeptLines);
        for (std::size_t i = lines_written - count; i < lines_written; i++) {
            report += recent_lines[i % KeptLines];
        }
    }

    ZipWriter zip{path};
    zip.AddText("report.txt", report);
    zip.AddTree("save/", base + "/sdmc/title/00040000/00126300/data");
    zip.AddTree("extdata/", base + "/sdmc/extdata/00000000/00001263");
    if (!dump.empty()) {
        zip.AddFile("crash/" + dump, "ux0:data/" + dump, true);
    }
    if (with_state && !state.empty()) {
        // Already compressed (zstd): stored as it is.
        zip.AddFile("states/" + state, base + "/states/" + state, false);
    }
    if (!zip.Finish()) {
        sceIoRemove(path.c_str());
        return "Could not write the report (is the memory card full?).";
    }
    SceIoStat st{};
    sceIoGetstat(path.c_str(), &st);
    const double mb = static_cast<double>(st.st_size) / (1024.0 * 1024.0);
    std::string message = fmt::format("Saved ux0:data/mh4u/reports/{} ({:.1f} MB).\n\n", file_name, mb);
    if (with_state && state.empty()) {
        message += "There was no save state to add.\n\n";
    }
    message += "Copy it to a PC (VitaShell) and attach it to an issue on the project's GitHub page "
               "with what happened and where.";
    if (mb > 24.0) {
        message += " It is over GitHub's 25 MB limit: share it through a cloud drive link.";
    }
    return message;
}

} // namespace VitaFrontend
