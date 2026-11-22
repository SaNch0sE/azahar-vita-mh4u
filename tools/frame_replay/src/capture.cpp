// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

#include "capture.h"

#include <format>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <zstd.h>

namespace frame_replay {

namespace {

constexpr u32 Tag(const char (&s)[5]) {
    return u32(u8(s[0])) | u32(u8(s[1])) << 8 | u32(u8(s[2])) << 16 | u32(u8(s[3])) << 24;
}

class Reader {
public:
    Reader(const u8* p, size_t n) : p(p), n(n) {}
    bool Has(size_t bytes) const {
        return at + bytes <= n;
    }
    u32 Word() {
        u32 v = 0;
        if (Has(4)) {
            std::memcpy(&v, p + at, 4);
        }
        at += 4;
        return v;
    }
    float Float() {
        const u32 v = Word();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    void Words(std::vector<u32>& out, size_t count) {
        out.resize(count);
        for (auto& w : out) {
            w = Word();
        }
    }
    std::string Rest() const {
        return at < n ? std::string(reinterpret_cast<const char*>(p + at), n - at) : std::string();
    }
    const u8* p;
    size_t n;
    size_t at = 0;
};

void ReadUniforms(Reader& r, Uniforms& u, bool delta) {
    if (delta) {
        const u32 n = r.Word();
        for (u32 k = 0; k < n; k++) {
            const u32 i = r.Word();
            for (u32 c = 0; c < 4; c++) {
                const float f = r.Float();
                if (i < 96) {
                    u.f[i][c] = f;
                }
            }
        }
    } else {
        for (auto& v : u.f) {
            for (auto& c : v) {
                c = r.Float();
            }
        }
    }
    u.b = r.Word();
    for (auto& i : u.i) {
        i = r.Word();
    }
}

} // namespace

const char* MemKindName(u32 kind) {
    static const char* names[] = {"command list", "vertices", "indices", "texture",
                                  "colour",       "depth",    "DMA source"};
    return kind < 7 ? names[kind] : "?";
}

int Capture::TagId(const std::string& name) const {
    for (size_t i = 0; i < tags.size(); i++) {
        if (tags[i] == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const u8* Capture::Blob(u32 id, size_t* size) const {
    const auto it = frame.blobs.find(id);
    if (it == frame.blobs.end()) {
        *size = 0;
        return nullptr;
    }
    *size = it->second.second;
    return data.data() + it->second.first;
}

bool Capture::Load(const std::string& file, std::string* error) {
    *this = Capture{};
    path = file;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        *error = "cannot open " + file;
        return false;
    }
    std::vector<u8> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() < 12) {
        *error = "too short";
        return false;
    }
    u32 head[3];
    std::memcpy(head, raw.data(), 12);
    if (head[0] == Tag("VPRZ")) {
        data.resize(head[2]);
        const size_t got = ZSTD_decompress(data.data(), data.size(), raw.data() + 12, raw.size() - 12);
        if (ZSTD_isError(got)) {
            *error = std::string("zstd: ") + ZSTD_getErrorName(got);
            return false;
        }
        data.resize(got);
    } else if (head[0] == Tag("VPRF")) {
        data = std::move(raw);
    } else {
        *error = "not a vprof capture";
        return false;
    }
    Reader top(data.data(), data.size());
    if (top.Word() != Tag("VPRF")) {
        *error = "not a vprof stream";
        return false;
    }
    // 1: CPUS cpu | seq << 2 (no guest bit), no GCOD; 2: cpu | guest << 2 | seq << 3;
    // 3: RSTA and PRES.
    version = top.Word();
    if (version < 1 || version > 3) {
        *error = "unknown stream version " + std::to_string(version);
        return false;
    }
    Frame& f = frame;
    Draw current; // the state the draw deltas apply to
    current.regs.assign(NumRegs, 0);
    int vs_program = -1, gs_program = -1;
    while (top.Has(8)) {
        const u32 tag = top.Word();
        const u32 size = top.Word();
        const size_t padded = (size_t(size) + 3) & ~size_t(3);
        if (!top.Has(padded)) {
            *error = "truncated record payload or padding";
            return false;
        }
        const size_t start = top.at;
        Reader r(data.data() + start, size);
        top.at = start + padded;
        switch (tag) {
        case Tag("INFO"): {
            r.Word();
            std::istringstream lines(r.Rest());
            std::string line;
            while (std::getline(lines, line)) {
                const auto eq = line.find('=');
                if (eq != std::string::npos) {
                    info[line.substr(0, eq)] = line.substr(eq + 1);
                }
            }
            break;
        }
        case Tag("THRD"): {
            const u32 tid = r.Word();
            threads[tid] = r.Rest();
            break;
        }
        case Tag("TAGN"): {
            const u32 id = r.Word();
            if (tags.size() <= id) {
                tags.resize(id + 1);
            }
            tags[id] = r.Rest();
            break;
        }
        case Tag("EVTS"): {
            for (u32 i = 0; i + 16 <= size; i += 16) {
                Event e;
                e.time = r.Word();
                e.tid = r.Word();
                e.tag = r.Word();
                e.arg = r.Word();
                events.push_back(e);
            }
            break;
        }
        case Tag("FRAM"):
            f.present = true;
            f.number = r.Word();
            f.start = r.Word();
            break;
        case Tag("VBLK"):
            r.Word();
            f.end = r.Word();
            break;
        case Tag("PSTA"): {
            PicaState& s = f.state;
            r.Words(s.regs, r.Word());
            r.Words(s.lcd, r.Word());
            for (int k = 0; k < 2; k++) {
                Program& p = k == 0 ? s.vs : s.gs;
                p.which = k;
                r.Words(p.code, 4096);
                r.Words(p.swizzle, 4096);
                Uniforms& u = k == 0 ? s.vs_u : s.gs_u;
                ReadUniforms(r, u, false);
                (k == 0 ? s.vs_entry : s.gs_entry) = r.Word();
            }
            for (auto& a : s.default_attributes) {
                for (auto& c : a) {
                    c = r.Float();
                }
            }
            r.Words(s.proctex, 128 * 3 + 256 * 2);
            r.Words(s.lighting, 24 * 256);
            r.Words(s.fog, 128);
            f.has_state = true;
            current.regs = s.regs;
            current.regs.resize(NumRegs);
            current.vs = s.vs_u;
            current.gs = s.gs_u;
            f.programs.push_back(s.vs);
            vs_program = static_cast<int>(f.programs.size() - 1);
            f.programs.push_back(s.gs);
            gs_program = static_cast<int>(f.programs.size() - 1);
            break;
        }
        case Tag("GSPC"): {
            GspCommand c;
            for (auto& w : c.words) {
                w = r.Word();
            }
            c.time = r.Word();
            f.gsp.push_back(c);
            f.stream.push_back({StreamItem::Gsp, f.gsp.size() - 1});
            break;
        }
        case Tag("EREG"): {
            RegWrite w;
            w.index = r.Word();
            w.value = r.Word();
            w.lcd = r.Word();
            w.time = r.Word();
            f.regs.push_back(w);
            f.stream.push_back({StreamItem::Reg, f.regs.size() - 1});
            break;
        }
        case Tag("EDMA"): {
            Dma d;
            d.time = r.Word();
            const u32 runs = r.Word();
            for (u32 i = 0; i < runs; i++) {
                d.runs.push_back({r.Word(), r.Word(), r.Word()});
            }
            f.dmas.push_back(d);
            f.stream.push_back({StreamItem::DmaOp, f.dmas.size() - 1});
            break;
        }
        case Tag("OPQ "): {
            const u32 seq = r.Word();
            Op& op = f.ops[seq];
            op.seq = seq;
            op.kind = r.Word();
            op.queued = r.Word();
            f.stream.push_back({StreamItem::OpQueued, seq});
            break;
        }
        case Tag("OPB "): {
            const u32 seq = r.Word();
            f.ops[seq].seq = seq;
            f.ops[seq].begin = r.Word();
            break;
        }
        case Tag("OPE "): {
            const u32 seq = r.Word();
            f.ops[seq].seq = seq;
            f.ops[seq].end = r.Word();
            break;
        }
        case Tag("MEML"): {
            MemLoad m;
            m.seq = r.Word();
            m.kind = r.Word();
            m.paddr = r.Word();
            m.size = r.Word();
            m.blob = r.Word();
            f.loads.push_back(m);
            f.ops[m.seq].loads.push_back(f.loads.size() - 1);
            break;
        }
        case Tag("PRES"): {
            Result x;
            x.seq = r.Word();
            x.index = r.Word();
            x.kind = r.Word();
            x.addr = r.Word();
            x.width = r.Word();
            x.height = r.Word();
            x.format = r.Word();
            x.blob = r.Word();
            f.results.push_back(x);
            break;
        }
        case Tag("RSTA"): {
            r.Word(); // version
            r.Word(); // clear order
            f.state_clears = r.Word();
            for (u32 k = 0; k < f.state_clears * 5; k++) {
                r.Word();
            }
            r.Word(); // generations
            f.state_surfaces = r.Word();
            for (u32 k = 0; k < f.state_surfaces * 10; k++) {
                r.Word();
            }
            f.state_depths = r.Word();
            f.has_rasterizer_state = true;
            break;
        }
        case Tag("PROG"): {
            Program p;
            p.which = r.Word();
            r.Words(p.code, 4096);
            r.Words(p.swizzle, 4096);
            f.programs.push_back(std::move(p));
            (f.programs.back().which == 0 ? vs_program : gs_program) =
                static_cast<int>(f.programs.size() - 1);
            break;
        }
        case Tag("DRAW"): {
            Draw d;
            d.seq = r.Word();
            d.pass = r.Word();
            d.draw = r.Word();
            d.vertices = r.Word();
            d.time = r.Word();
            const u32 n = r.Word();
            for (u32 k = 0; k < n; k++) {
                const u32 i = r.Word();
                const u32 v = r.Word();
                if (i < NumRegs) {
                    current.regs[i] = v;
                    d.changed.push_back(i);
                }
            }
            ReadUniforms(r, current.vs, true);
            ReadUniforms(r, current.gs, true);
            d.regs = current.regs;
            d.vs = current.vs;
            d.gs = current.gs;
            d.vs_program = vs_program;
            d.gs_program = gs_program;
            f.draws.push_back(std::move(d));
            f.ops[f.draws.back().seq].draws.push_back(f.draws.size() - 1);
            break;
        }
        case Tag("PASS"): {
            Pass p;
            p.seq = r.Word();
            p.pass = r.Word();
            p.target = r.Word();
            const u32 wh = r.Word();
            p.width = wh & 0xffff;
            p.height = wh >> 16;
            p.draws = r.Word();
            p.sync = r.Word();
            p.time = r.Word();
            f.passes.push_back(p);
            f.ops[p.seq].passes.push_back(f.passes.size() - 1);
            break;
        }
        case Tag("PTIM"): {
            r.Word(); // seq
            const u32 pass = r.Word();
            const u32 ta_us = r.Word();
            const u32 d3_us = r.Word();
            for (auto it = f.passes.rbegin(); it != f.passes.rend(); ++it) {
                if (it->pass == pass) {
                    it->ta_us = ta_us;
                    it->d3_us = d3_us;
                    it->timed = true;
                    break;
                }
            }
            break;
        }
        case Tag("BLOB"): {
            const u32 id = r.Word();
            f.blobs[id] = {start + 4, size - 4};
            break;
        }
        default:
            break;
        }
    }
    if (top.at != top.n) {
        *error = "truncated record header";
        return false;
    }
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.time < b.time; });
    t0 = ~0u;
    t1 = 0;
    for (const auto& e : events) {
        t0 = std::min(t0, e.time);
        t1 = std::max(t1, e.time);
    }
    if (t0 > t1) {
        t0 = t1 = 0;
    }
    return true;
}

} // namespace frame_replay
