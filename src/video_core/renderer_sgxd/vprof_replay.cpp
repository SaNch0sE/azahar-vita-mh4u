// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_sgxd/vprof_replay.h"

#ifdef VITA_DIAGNOSTICS

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/processmgr.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/zstd_compression.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"
#include "video_core/renderer_sgxd/vprof.h"
#include "sgxd.h"

namespace SgxdRenderer::Vprof {

namespace {

constexpr u32 Tag(const char (&s)[5]) {
    return u32(u8(s[0])) | u32(u8(s[1])) << 8 | u32(u8(s[2])) << 16 | u32(u8(s[3])) << 24;
}

constexpr u32 InternalBase = 0x400;

struct Item {
    u32 kind; // 0 GSP command, 1 register write, 2 DMA
    std::vector<u32> words;
    std::vector<u32> ops; // the ops it queued
};

struct Load {
    u32 kind, paddr, size, blob;
    const u8* data = nullptr;
    // The same bytes at the same place as an earlier load of the op, with no load between
    // them over any of it: the op's draws re-read a texture draw after draw, and the earlier
    // load has already put it there.
    bool repeat = false;
};

struct ReplayFrame {
    std::vector<u8> bytes;
    const u8* psta = nullptr;
    std::size_t psta_size = 0;
    std::vector<Item> items;
    u32 state_op = 0;
    std::map<u32, std::vector<Load>> loads;
    std::vector<u32> draw_op;     // per draw of the frame
    std::vector<u32> draw_colour; // its colour buffer
    std::vector<u32> draw_pass;   // the rasterizer's pass number in the capture
    // The rasterizer's state at the frame's start (RSTA; captures from version 3).
    bool has_state = false;
    RasterizerSgxd::VprofState state;
    std::vector<u32> surface_blobs, depth_blobs; // resolved into state once the blobs are read
};

class Writer {
public:
    std::vector<u8> bytes;
    void Word(u32 v) {
        const std::size_t at = bytes.size();
        bytes.resize(at + 4);
        std::memcpy(bytes.data() + at, &v, 4);
    }
    void Raw(const void* p, std::size_t n) {
        const u8* b = static_cast<const u8*>(p);
        bytes.insert(bytes.end(), b, b + n);
    }
    std::size_t Begin(u32 tag) {
        Word(tag);
        Word(0);
        return bytes.size();
    }
    void End(std::size_t start) {
        const u32 size = static_cast<u32>(bytes.size() - start);
        std::memcpy(bytes.data() + start - 4, &size, 4);
        while (bytes.size() % 4) {
            bytes.push_back(0);
        }
    }
    void Text(const std::string& t) {
        const std::size_t r = Begin(Tag("RTXT"));
        Raw(t.data(), t.size());
        End(r);
    }
};

struct Replay {
    ReplayFrame frame;
    bool loaded = false;
    std::atomic<bool> active{false};
    u32 next_op = 0;
    ReplayHost* host = nullptr;
    // Guest memory the replay wrote over, as it was (restored at quit).
    std::map<std::pair<u32, u32>, std::vector<u8>> saved;
    u32 diff_loads = 0, diff_bytes = 0;
    std::map<u32, std::pair<u64, u64>> op_times; // seq -> begin, end
    // The `results` request: every pass's and transfer's result of one run (RPRS).
    bool collect = false;
    struct Result {
        u32 kind, addr, width, height, format;
        std::vector<u8> bytes;
    };
    std::vector<Result> results;
    // The time ReplayOpBegin spent on memory loads since the last reset (`profile`).
    u64 load_us = 0;
};

Replay& R() {
    static Replay r;
    return r;
}

u32 Get(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

bool Parse(std::vector<u8> file, ReplayFrame& f, std::string& err) {
    if (file.size() < 12) {
        err = "short file";
        return false;
    }
    if (Get(file.data()) == Tag("VPRZ")) {
        f.bytes = Common::Compression::DecompressDataZSTD(
            std::span<const u8>(file.data() + 12, file.size() - 12));
    } else {
        f.bytes = std::move(file);
    }
    const u8* d = f.bytes.data();
    const std::size_t n = f.bytes.size();
    if (n < 8 || Get(d) != Tag("VPRF")) {
        err = "not a capture";
        return false;
    }
    std::map<u32, const u8*> blobs;
    std::vector<u32> regs(0x732, 0);
    bool in_frame = false;
    for (std::size_t at = 8; at + 8 <= n;) {
        const u32 tag = Get(d + at), size = Get(d + at + 4);
        const u8* p = d + at + 8;
        if (at + 8 + size > n) {
            break;
        }
        at += 8 + ((size + 3) & ~3u);
        const auto w = [p](u32 i) { return Get(p + i * 4); };
        switch (tag) {
        case Tag("FRAM"):
            in_frame = true;
            break;
        case Tag("PSTA"): {
            f.psta = p;
            f.psta_size = size;
            const u32 count = std::min<u32>(w(0), 0x732);
            for (u32 i = 0; i < count; i++) {
                regs[i] = w(1 + i);
            }
            break;
        }
        case Tag("GSPC"):
            f.items.push_back({0, std::vector<u32>(8), {}});
            for (u32 i = 0; i < 8; i++) {
                f.items.back().words[i] = w(i);
            }
            break;
        case Tag("EREG"):
            f.items.push_back({1, {w(0), w(1), w(2)}, {}});
            break;
        case Tag("EDMA"): {
            Item it{2, {}, {}};
            const u32 runs = w(1);
            for (u32 i = 0; i < runs * 3; i++) {
                it.words.push_back(w(2 + i));
            }
            f.items.push_back(std::move(it));
            break;
        }
        case Tag("OPQ "):
            if (f.items.empty()) {
                f.state_op = w(0); // the vblank's state op, before any command
            } else {
                f.items.back().ops.push_back(w(0));
            }
            break;
        case Tag("MEML"):
            f.loads[w(0)].push_back({w(1), w(2), w(3), w(4)});
            break;
        case Tag("DRAW"): {
            f.draw_op.push_back(w(0));
            f.draw_pass.push_back(w(1));
            const u32 changed = w(5);
            for (u32 k = 0; k < changed; k++) {
                const u32 i = w(6 + k * 2);
                if (i < regs.size()) {
                    regs[i] = w(7 + k * 2);
                }
            }
            f.draw_colour.push_back((regs[InternalBase + 0x11D] & 0x0FFFFFFF) * 8);
            break;
        }
        case Tag("RSTA"): {
            // FORMAT.md: version, the clears, the colour surfaces, the depth stores.
            auto& st = f.state;
            u32 i = 1;
            st.clear_order = w(i++);
            const u32 clears = w(i++);
            for (u32 k = 0; k < clears; k++, i += 5) {
                st.clears.push_back({w(i), w(i + 1), w(i + 2), w(i + 3), w(i + 4)});
            }
            st.generations = w(i++);
            const u32 colours = w(i++);
            for (u32 k = 0; k < colours; k++, i += 10) {
                st.surfaces.push_back({w(i), w(i + 1), w(i + 2), w(i + 3), w(i + 4), w(i + 5),
                                       w(i + 8), w(i + 6) != 0, w(i + 7) != 0, nullptr});
                f.surface_blobs.push_back(w(i + 9));
            }
            const u32 depths = w(i++);
            for (u32 k = 0; k < depths; k++, i += 7) {
                st.depths.push_back({w(i), w(i + 1), w(i + 2), w(i + 3), w(i + 4) != 0, nullptr,
                                     w(i + 5)});
                f.depth_blobs.push_back(w(i + 6));
            }
            f.has_state = true;
            break;
        }
        case Tag("BLOB"):
            blobs[w(0)] = p + 4;
            break;
        default:
            break;
        }
    }
    (void)in_frame;
    const auto blob = [&blobs](u32 id) -> const u8* {
        const auto it = blobs.find(id);
        return it != blobs.end() ? it->second : nullptr;
    };
    for (std::size_t k = 0; k < f.state.surfaces.size(); k++) {
        f.state.surfaces[k].pixels = blob(f.surface_blobs[k]);
    }
    for (std::size_t k = 0; k < f.state.depths.size(); k++) {
        f.state.depths[k].samples = blob(f.depth_blobs[k]);
        if (!f.state.depths[k].samples) {
            f.state.depths[k].valid = false;
        }
    }
    // A surface whose pixels are missing is left out: a pass over it loads guest memory.
    std::erase_if(f.state.surfaces, [](const auto& c) { return c.pixels == nullptr; });
    for (auto& [seq, list] : f.loads) {
        for (Load& l : list) {
            // ~0u: the recording stopped keeping bytes at its budget (vprof.cpp FrameBlobBudget).
            if (l.blob == ~0u) {
                err = "the recorded frame read more than vprof keeps: it cannot be replayed";
                return false;
            }
            const auto it = blobs.find(l.blob);
            l.data = it != blobs.end() ? it->second : nullptr;
        }
        for (std::size_t j = 0; j < list.size(); j++) {
            Load& l = list[j];
            for (std::size_t i = j; i-- > 0;) {
                const Load& e = list[i];
                if (e.paddr == l.paddr && e.size == l.size && e.blob == l.blob) {
                    l.repeat = true;
                    break;
                }
                if (e.paddr < l.paddr + l.size && l.paddr < e.paddr + e.size) {
                    break;
                }
            }
        }
    }
    if (!f.psta || f.state_op == 0) {
        err = "the capture has no recorded frame";
        return false;
    }
    return true;
}

// The PICA's state from the capture's PSTA (FORMAT.md).
void RestoreState(Pica::PicaCore& pica, const ReplayFrame& f) {
    const u8* p = f.psta;
    std::size_t at = 0;
    const auto word = [&] {
        const u32 v = at + 4 <= f.psta_size ? Get(p + at) : 0;
        at += 4;
        return v;
    };
    const auto flt = [&] {
        const u32 v = word();
        float x;
        std::memcpy(&x, &v, 4);
        return x;
    };
    const u32 nregs = word();
    for (u32 i = 0; i < nregs; i++) {
        const u32 v = word();
        if (i < pica.regs.reg_array.size()) {
            pica.regs.reg_array[i] = v;
        }
    }
    const u32 nlcd = word();
    for (u32 i = 0; i < nlcd; i++) {
        const u32 v = word();
        if (i < Pica::RegsLcd::NumIds()) {
            pica.regs_lcd[static_cast<int>(i)] = v;
        }
    }
    for (Pica::ShaderSetup* setup : {&pica.vs_setup, &pica.gs_setup}) {
        Pica::ProgramCode code;
        for (auto& c : code) {
            c = word();
        }
        Pica::SwizzleData swizzle;
        for (auto& s : swizzle) {
            s = word();
        }
        setup->UpdateProgramCode(code);
        setup->UpdateSwizzleData(swizzle);
        for (auto& v : setup->uniforms.f) {
            for (u32 c = 0; c < 4; c++) {
                v[c] = Pica::f24::FromFloat32(flt());
            }
        }
        const u32 b = word();
        for (u32 i = 0; i < 16; i++) {
            setup->uniforms.b[i] = (b >> i) & 1;
        }
        for (auto& v : setup->uniforms.i) {
            const u32 x = word();
            v = Common::Vec4<u8>(u8(x), u8(x >> 8), u8(x >> 16), u8(x >> 24));
        }
        setup->entry_point = word();
        setup->uniforms_dirty = true;
    }
    for (auto& v : pica.input_default_attributes) {
        for (u32 c = 0; c < 4; c++) {
            v[c] = Pica::f24::FromFloat32(flt());
        }
    }
    const auto raw = [&](void* dst, std::size_t bytes) {
        if (at + bytes <= f.psta_size) {
            std::memcpy(dst, p + at, bytes);
        }
        at += bytes;
    };
    auto& pt = pica.proctex;
    raw(pt.noise_table.data(), sizeof(pt.noise_table));
    raw(pt.color_map_table.data(), sizeof(pt.color_map_table));
    raw(pt.alpha_map_table.data(), sizeof(pt.alpha_map_table));
    raw(pt.color_table.data(), sizeof(pt.color_table));
    raw(pt.color_diff_table.data(), sizeof(pt.color_diff_table));
    pt.table_dirty = Pica::PicaCore::ProcTex::TableAllDirty;
    raw(pica.lighting.luts.data(), sizeof(pica.lighting.luts));
    pica.lighting.lut_dirty = Pica::PicaCore::Lighting::LutAllDirty;
    raw(pica.fog.lut.data(), sizeof(pica.fog.lut));
    // All of it marked, for the render side's PicaCore, which takes it whole (state_restored).
    pica.MarkAllChanged();
}

// One replay of the frame: every item, or those up to the one that queues op `stop` (0: all),
// with the draws after `limit` left out (-1: none). The passes run synchronously, or with `sync`
// false pipelined as the game's are, and the textures kept from the run before (the game's
// steady state; a synchronous run untiles them all again). `begin` is when the frame's own
// items start (after the restored state), `submitted` when the last of them has been handed
// over.
void RunFrame(int limit, u32 stop, bool sync = true, u64* begin = nullptr,
              u64* submitted = nullptr) {
    Replay& r = R();
    ReplayHost& h = *r.host;
    h.sync([&] {
        h.rasterizer.ReplayBegin(limit);
        h.rasterizer.Backend().replay_sync = sync;
        h.rasterizer.Backend().replay_times.clear();
        h.rasterizer.Backend().side_times = {};
    });
    r.op_times.clear();
    r.results.clear();
    // Without it a run after the first would draw the right eye the first run blocked.
    h.frame_start();
    // The rasterizer as the frame found it on the console.
    if (r.frame.has_state) {
        h.sync([&h, &r, sync] { h.rasterizer.ReplayRestore(r.frame.state, sync); });
    }
    r.next_op = r.frame.state_op;
    r.active = true;
    h.op([&h, &r] {
        RestoreState(h.pica, r.frame);
        h.state_restored();
    });
    if (begin) {
        *begin = sceKernelGetProcessTimeWide();
    }
    for (const Item& it : r.frame.items) {
        if (stop && std::any_of(it.ops.begin(), it.ops.end(), [stop](u32 s) { return s > stop; })) {
            break;
        }
        switch (it.kind) {
        case 0:
            h.gsp(it.words.data());
            break;
        case 1:
            h.reg(it.words[0], it.words[1], it.words[2] != 0);
            break;
        case 2:
            h.dma(it.words);
            break;
        }
    }
    h.sync([&h] { h.rasterizer.ReplayEndPass(); });
    if (submitted) {
        *submitted = sceKernelGetProcessTimeWide();
    }
    r.active = false;
}

// The frame n times as the game runs it: its passes pipelined, each run ended by the GPU going
// idle, and with `present` the screens shown after it as a vblank shows them. Per run (RFRM):
// the time to hand the frame over (the render thread's work, the memory loads included), the
// time until the GPU is idle, the memory loads' part, the present's. A capture of the events runs
// over all of them (Vprof::ProfileStart), a host0:vprof-N.vprof of its own: the runs' GPU
// timeline (frame-replay trace).
void Profile(Writer& w, u32 n, bool present) {
    Replay& r = R();
    ReplayHost& h = *r.host;
    const bool sampled = ProfileStart(h.memory);
    std::vector<u32> submit, idle, loads, shown;
    for (u32 i = 0; i < n; i++) {
        r.load_us = 0;
        u64 t0 = 0, t1 = 0;
        RunFrame(-1, 0, false, &t0, &t1);
        h.sync([&h] { h.rasterizer.Backend().RenderIdle("vprof replay: the run's end"); });
        const u64 t2 = sceKernelGetProcessTimeWide();
        u64 t3 = t2;
        if (present && h.present) {
            h.present();
            t3 = sceKernelGetProcessTimeWide();
        }
        submit.push_back(static_cast<u32>(t1 - t0));
        idle.push_back(static_cast<u32>(t2 - t0));
        loads.push_back(static_cast<u32>(r.load_us));
        shown.push_back(static_cast<u32>(t3 - t2));
        const std::size_t rec = w.Begin(Tag("RFRM"));
        for (const u32 v : {i, submit.back(), idle.back(), loads.back(), shown.back()}) {
            w.Word(v);
        }
        w.End(rec);
    }
    if (sampled) {
        ProfileStop();
    }
    const auto median = [](std::vector<u32> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0u : v[v.size() / 2];
    };
    const auto low = [](const std::vector<u32>& v) {
        return v.empty() ? 0u : *std::min_element(v.begin(), v.end());
    };
    w.Text(fmt::format("profile {}: to idle median {} us (best {}), handed over {} us (best {}), "
                       "memory loads {} us, present {} us; {}",
                       n, median(idle), low(idle), median(submit), low(submit), median(loads),
                       median(shown), sampled ? "vprof sampled the runs" : "no vprof samples"));
}

void Restore() {
    Replay& r = R();
    for (const auto& [key, bytes] : r.saved) {
        if (u8* p = r.host->memory.GetPhysicalPointer(key.first)) {
            std::memcpy(p, bytes.data(), bytes.size());
        }
    }
    r.saved.clear();
}

void Finish(Writer& w, u32 number) {
    std::vector<u8> packed = Common::Compression::CompressDataZSTDDefault(w.bytes);
    const std::string path = fmt::format("host0:vprof-replay-out-{}.bin", number);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        const u32 head[3] = {Tag("VPRY"), 1, static_cast<u32>(w.bytes.size())};
        std::fwrite(head, 4, 3, f);
        std::fwrite(packed.data(), 1, packed.size(), f);
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen(fmt::format("host0:vprof-replay-done-{}", number).c_str(), "wb")) {
        std::fclose(f);
    }
    LOG_INFO(Render, "vprof replay: {} written ({} KiB)", path, packed.size() >> 10);
}

void Times(Writer& w, u32 run) {
    Replay& r = R();
    // The render side's, copied there.
    std::vector<std::array<u32, 3>> times;
    std::array<u32, 3> side{};
    r.host->sync([&] {
        times = r.host->rasterizer.Backend().replay_times;
        side = r.host->rasterizer.Backend().side_times;
    });
    for (u32 i = 0; i < times.size(); i++) {
        const std::size_t rec = w.Begin(Tag("RPAS"));
        w.Word(run);
        w.Word(i);
        w.Word(times[i][1]);
        w.Word(times[i][2]);
        w.End(rec);
    }
    // sgxd_flags syncside: the side renders' sums, one more pass after the frame's.
    if (side[0] != 0) {
        const std::size_t rec = w.Begin(Tag("RPAS"));
        w.Word(run);
        w.Word(static_cast<u32>(times.size()));
        w.Word(side[1]);
        w.Word(side[2]);
        w.End(rec);
    }
    for (const auto& [seq, t] : r.op_times) {
        const std::size_t rec = w.Begin(Tag("ROPT"));
        w.Word(run);
        w.Word(seq);
        w.Word(static_cast<u32>(t.second - t.first));
        w.End(rec);
    }
}

void Image(Writer& w, u32 draw) {
    Replay& r = R();
    if (draw >= r.frame.draw_op.size()) {
        return;
    }
    RunFrame(static_cast<int>(draw), r.frame.draw_op[draw]);
    std::vector<u8> px;
    u32 width = 0, height = 0;
    bool ok = false;
    r.host->sync([&] {
        ok = r.host->rasterizer.ReplayReadSurface(r.frame.draw_colour[draw], px, width, height);
    });
    const std::size_t rec = w.Begin(Tag("RIMG"));
    w.Word(draw);
    w.Word(ok ? width : 0);
    w.Word(ok ? height : 0);
    w.Word(r.frame.draw_colour[draw]);
    if (ok) {
        w.Raw(px.data(), px.size());
    }
    w.End(rec);
}

// Whether guest memory at paddr (p on the CPU) holds size bytes of data. Guest VRAM is uncached
// for the CPU, whose reads of it run at about 13 MB/s: the DMAC copies it into a cached buffer
// instead.
bool Same(const u8* p, u32 paddr, const u8* data, u32 size) {
    if (paddr < Memory::VRAM_PADDR || paddr + u64{size} > Memory::VRAM_PADDR + Memory::VRAM_SIZE) {
        return std::memcmp(p, data, size) == 0;
    }
    constexpr u32 Chunk = 256 * 1024;
    alignas(64) static std::array<u8, Chunk> bounce;
    // The CPU only reads the buffer: once no line of it is dirty, none is written back over
    // what the DMAC puts there, and only the lines a copy filled need invalidating after it.
    static const bool clean = [] {
        SgxdCacheRange(bounce.data(), Chunk, SGXD_CACHE_CLEAN_INVALIDATE, CacheSite::Other);
        return true;
    }();
    (void)clean;
    for (u32 at = 0; at < size; at += Chunk) {
        const u32 n = std::min(Chunk, size - at);
        const int copied = sceDmacMemcpy(bounce.data(), p + at, n);
        ASSERT_MSG(copied >= 0, "vprof replay: sceDmacMemcpy of guest VRAM {:#x}: {:#010x}",
                   paddr + at, static_cast<u32>(copied));
        SgxdCacheRange(bounce.data(), (n + 63) & ~63u, SGXD_CACHE_INVALIDATE, CacheSite::Other);
        if (std::memcmp(bounce.data(), data + at, n) != 0) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ReplayActive() {
    return R().active.load(std::memory_order_relaxed);
}

u32 ReplayOpQueued() {
    return R().next_op++;
}

void ReplayOpBegin(u32 seq) {
    Replay& r = R();
    const u64 now = sceKernelGetProcessTimeWide();
    r.op_times[seq].first = now;
    const auto it = r.frame.loads.find(seq);
    if (it == r.frame.loads.end()) {
        return;
    }
    struct LoadTime {
        Replay& r;
        u64 t0;
        ~LoadTime() {
            r.load_us += sceKernelGetProcessTimeWide() - t0;
        }
    } timed{r, now};
    std::vector<std::pair<u32, u32>> written;
    for (const Load& l : it->second) {
        // Guest memory as the console held it when the op ran: command lists, vertices,
        // indices, textures, DMA sources, and the colour and depth a pass starts from (read
        // only where the restored state has no surface or store for them).
        if (!l.data || l.repeat) {
            continue;
        }
        u8* p = r.host->memory.GetPhysicalPointer(l.paddr);
        if (!p || Same(p, l.paddr, l.data, l.size)) {
            continue;
        }
        const auto key = std::make_pair(l.paddr, l.size);
        if (!r.saved.count(key)) {
            r.saved[key].assign(p, p + l.size);
        }
        std::memcpy(p, l.data, l.size);
        written.emplace_back(l.paddr, l.paddr + l.size);
        r.diff_loads++;
        r.diff_bytes += l.size;
    }
    // The rasterizer's, on the render thread (the ops before this one have run there).
    if (!written.empty()) {
        r.host->sync([&] {
            for (const auto& [lo, hi] : written) {
                r.host->rasterizer.ReplayMemoryWritten(lo, hi);
            }
        });
    }
}

bool ReplayWantsResults() {
    return R().collect;
}

void ReplayResult(u32 kind, u32 addr, u32 width, u32 height, u32 format, const u8* data,
                  u32 size) {
    Replay& r = R();
    if (!r.collect) {
        return;
    }
    r.results.push_back({kind, addr, width, height, format, std::vector<u8>(data, data + size)});
}

void ReplayOpEnd(u32 seq) {
    R().op_times[seq].second = sceKernelGetProcessTimeWide();
}

void Serve(ReplayHost& host, const std::string& capture, const std::string& first) {
    Replay& r = R();
    r.host = &host;
    if (!r.loaded) {
        std::vector<u8> file;
        if (std::FILE* f = std::fopen(capture.c_str(), "rb")) {
            u8 buf[65536];
            while (const std::size_t n = std::fread(buf, 1, sizeof(buf), f)) {
                file.insert(file.end(), buf, buf + n);
            }
            std::fclose(f);
        }
        std::string err;
        if (!Parse(std::move(file), r.frame, err)) {
            LOG_ERROR(Render, "vprof replay: {}: {}", capture, err);
            return;
        }
        r.loaded = true;
        LOG_INFO(Render, "vprof replay: {} commands, {} draws; the guest waits", r.frame.items.size(),
                 r.frame.draw_op.size());
    }
    for (u32 number = 0;; number++) {
        std::string text = number == 0 ? first : std::string{};
        while (text.empty()) {
            if (std::FILE* f = std::fopen("host0:vprof-replay.txt", "rb")) {
                char buf[4096];
                while (const std::size_t n = std::fread(buf, 1, sizeof(buf), f)) {
                    text.append(buf, n);
                }
                std::fclose(f);
                std::remove("host0:vprof-replay.txt");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        Writer w;
        w.Word(Tag("VPRR"));
        w.Word(3); // 2: RPRS; 3: RFRM
        {
            const std::size_t rec = w.Begin(Tag("RINF"));
            w.Word(static_cast<u32>(r.frame.draw_op.size()));
            w.End(rec);
        }
        bool quit = false;
        std::istringstream lines(text);
        std::string line;
        u32 run = 0;
        while (std::getline(lines, line)) {
            std::istringstream in(line);
            std::string cmd;
            in >> cmd;
            const u64 t0 = sceKernelGetProcessTimeWide();
            if (cmd == "run") {
                u32 n = 1;
                in >> n;
                for (u32 i = 0; i < std::max(1u, n); i++) {
                    r.diff_loads = r.diff_bytes = 0;
                    RunFrame(-1, 0);
                    Times(w, run++);
                }
                w.Text(fmt::format("run {}: memory loads that differed {} ({} bytes)", n,
                                   r.diff_loads, r.diff_bytes));
            } else if (cmd == "costs") {
                // `costs FIRST LAST`: those draws alone (each a run of the frame up to it, best
                // of three, the memory loaded each time).
                const u32 draws = static_cast<u32>(r.frame.draw_op.size());
                u32 first = 0, last = draws - 1;
                if (in >> first) {
                    in >> last;
                }
                for (u32 d = first; d <= std::min(last, draws - 1); d++) {
                    u32 best_3d = ~0u, best_ta = ~0u;
                    u32 pass_index = 0;
                    for (u32 k = 0; k < 3; k++) {
                        RunFrame(static_cast<int>(d), r.frame.draw_op[d]);
                        std::vector<std::array<u32, 3>> times;
                        host.sync([&] { times = host.rasterizer.Backend().replay_times; });
                        if (!times.empty()) {
                            best_ta = std::min(best_ta, times.back()[1]);
                            best_3d = std::min(best_3d, times.back()[2]);
                            pass_index = static_cast<u32>(times.size() - 1);
                        }
                    }
                    const std::size_t rec = w.Begin(Tag("RCST"));
                    w.Word(d);
                    w.Word(best_ta);
                    w.Word(best_3d);
                    w.Word(pass_index);
                    w.End(rec);
                }
            } else if (cmd == "image") {
                u32 d = 0;
                while (in >> d) {
                    Image(w, d);
                }
            } else if (cmd == "images") {
                for (u32 d = 0; d < r.frame.draw_op.size(); d++) {
                    Image(w, d);
                }
            } else if (cmd == "results") {
                r.diff_loads = r.diff_bytes = 0;
                r.collect = true;
                RunFrame(-1, 0);
                r.collect = false;
                for (u32 i = 0; i < r.results.size(); i++) {
                    const Replay::Result& res = r.results[i];
                    const std::size_t rec = w.Begin(Tag("RPRS"));
                    for (const u32 v : {i, res.kind, res.addr, res.width, res.height, res.format,
                                        static_cast<u32>(res.bytes.size())}) {
                        w.Word(v);
                    }
                    w.Raw(res.bytes.data(), res.bytes.size());
                    w.End(rec);
                }
                w.Text(fmt::format("results: {} (the frame {} the console's state; memory loads "
                                   "that differed {}, {} bytes)",
                                   r.results.size(),
                                   r.frame.has_state ? "started from" : "had no record of",
                                   r.diff_loads, r.diff_bytes));
                r.results.clear();
            } else if (cmd == "scale") {
                // `scale Q`: the internal resolution in quarters for the runs after it (4 1x, 5
                // 1.25x, 6 1.5x), as the setting gives it: every buffer that scales whole is
                // drawn into a picture at it.
                u32 quarters = 4;
                in >> quarters;
                RasterizerSgxd::SetTopScale(quarters);
                w.Text(fmt::format("scale: {}/4", RasterizerSgxd::TopScale()));
            } else if (cmd == "profile") {
                u32 n = 10;
                std::string option;
                in >> n >> option;
                Profile(w, std::max(1u, n), option == "present");
            } else if (cmd == "quit") {
                quit = true;
            } else if (!cmd.empty()) {
                w.Text("unknown request: " + line);
            }
            if (!cmd.empty()) {
                w.Text(fmt::format("{}: {:.1f} ms", line,
                                   (sceKernelGetProcessTimeWide() - t0) / 1000.0));
            }
        }
        host.sync([&host] {
            host.rasterizer.ReplayStop();
            host.rasterizer.Backend().replay_sync = false;
        });
        Finish(w, number);
        if (quit) {
            Restore();
            LOG_INFO(Render, "vprof replay: done, guest memory restored; the guest goes on");
            return;
        }
    }
}

} // namespace SgxdRenderer::Vprof

#endif
