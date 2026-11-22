// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

#include "replay.h"

#include <cstring>
#include <format>
#include <fstream>
#include <zstd.h>

namespace frame_replay {

namespace {

constexpr u32 Tag(const char (&s)[5]) {
    return u32(u8(s[0])) | u32(u8(s[1])) << 8 | u32(u8(s[2])) << 16 | u32(u8(s[3])) << 24;
}

u32 Get(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

std::map<const Capture*, ReplayResults>& All() {
    static std::map<const Capture*, ReplayResults> all;
    return all;
}

std::string ResultPath(const Capture* cap, const std::string& where) {
    std::string p = cap->path;
    if (p.size() > 6 && p.ends_with(".vprof")) {
        p.resize(p.size() - 6);
    }
    return p + "." + where + ".bin";
}

} // namespace

bool LoadReplays(const Capture* cap, std::string* error, const std::string& where) {
    const std::string path = ResultPath(cap, where);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        *error = "no replay results (" + path + ")";
        return false;
    }
    std::vector<u8> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 12 || Get(file.data()) != Tag("VPRY")) {
        *error = path + ": not replay results";
        return false;
    }
    std::vector<u8> d(Get(file.data() + 8));
    const size_t n = ZSTD_decompress(d.data(), d.size(), file.data() + 12, file.size() - 12);
    if (ZSTD_isError(n) || n != d.size() || n < 8 || Get(d.data()) != Tag("VPRR")) {
        *error = path + ": cannot unpack";
        return false;
    }
    ReplayResults r;
    r.path = path;
    r.where = where;
    for (size_t at = 8; at + 8 <= n;) {
        const u32 tag = Get(&d[at]), size = Get(&d[at + 4]);
        const u8* p = &d[at + 8];
        if (at + 8 + size > n) {
            break;
        }
        at += 8 + ((size + 3) & ~3u);
        const auto w = [p](u32 i) { return Get(p + i * 4); };
        switch (tag) {
        case Tag("RINF"):
            r.draws = w(0);
            break;
        case Tag("RTXT"):
            r.text.emplace_back(reinterpret_cast<const char*>(p), size);
            break;
        case Tag("RPAS"):
            r.passes.push_back({w(0), w(1), w(2), w(3)});
            break;
        case Tag("ROPT"):
            r.ops.push_back({w(0), w(1), w(2)});
            break;
        case Tag("RCST"):
            r.costs[w(0)] = {w(1), w(2), w(3)};
            break;
        case Tag("RIMG"): {
            ReplayResults::Image im;
            im.width = w(1);
            im.height = w(2);
            im.addr = w(3);
            const size_t bytes = size_t(im.width) * im.height * 4;
            if (bytes && 16 + bytes <= size) {
                im.rgba.assign(p + 16, p + 16 + bytes);
            }
            r.images[w(0)] = std::move(im);
            break;
        }
        case Tag("RPRS"): {
            ReplayResults::Result x{w(0), w(1), w(2), w(3), w(4), w(5), {}};
            const u32 bytes = w(6);
            if (28 + size_t(bytes) <= size) {
                x.bytes.assign(p + 28, p + 28 + bytes);
            }
            r.results.push_back(std::move(x));
            break;
        }
        case Tag("RFRM"):
            r.runs.push_back({w(0), w(1), w(2), w(3), w(4)});
            break;
        default:
            break;
        }
    }
    All()[cap] = std::move(r);
    return true;
}

const ReplayResults* Replays(const Capture* cap) {
    const auto it = All().find(cap);
    return it == All().end() ? nullptr : &it->second;
}

} // namespace frame_replay
