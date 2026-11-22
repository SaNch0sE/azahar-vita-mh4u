// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/renderer_sgxd/vprof.h"
#include "video_core/renderer_sgxd/vprof_replay.h"

#ifdef VITA_DIAGNOSTICS

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include "common/assert.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/zstd_compression.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"

namespace SgxdRenderer::Vprof {

namespace {

// FORMAT.md's record tags: four ASCII bytes, little-endian.
constexpr u32 Tag(const char (&s)[5]) {
    return u32(u8(s[0])) | u32(u8(s[1])) << 8 | u32(u8(s[2])) << 16 | u32(u8(s[3])) << 24;
}

constexpr u32 FileVersion = 3; // 3: RSTA and PRES
/// The capture's records live in one main-memory block of their own, not the nearly full heap.
/// The regions: the file's records before the frame, the frame's records, and the guest memory
/// its ops read (blobs). A range past the blobs' region is recorded with NoBlob.
constexpr std::size_t HeadRegion = 8u << 20; // the events (4 MiB at most), threads, tags
constexpr std::size_t FrameRegion = 16u << 20;
constexpr std::size_t BlobRegion = 80u << 20;
constexpr u32 NoBlob = ~0u;

u32 Now() {
    return static_cast<u32>(sceKernelGetSystemTimeWide());
}

/// A record stream in a fixed region of the capture's block. Writing past its end stops the
/// emulator, naming the region.
class Writer {
public:
    void Attach(u8* base_, std::size_t capacity_, const char* name_) {
        base = base_;
        capacity = capacity_;
        name = name_;
        used = 0;
    }
    void Word(u32 v) {
        Raw(&v, 4);
    }
    void Float(float f) {
        u32 v;
        std::memcpy(&v, &f, 4);
        Word(v);
    }
    void Raw(const void* data, std::size_t size) {
        ASSERT_MSG(used + size <= capacity, "vprof: the {} region ({} MiB) is full", name,
                   capacity >> 20);
        std::memcpy(base + used, data, size);
        used += size;
    }
    /// A record: tag, payload size, the payload padded to four bytes.
    std::size_t Begin(u32 tag) {
        Word(tag);
        Word(0);
        return used;
    }
    void End(std::size_t start) {
        const u32 size = static_cast<u32>(used - start);
        std::memcpy(base + start - 4, &size, 4);
        static constexpr u8 zeros[4] = {};
        Raw(zeros, (4 - used % 4) % 4);
    }
    void Text(u32 tag, u32 head, const std::string& text) {
        const std::size_t s = Begin(tag);
        Word(head);
        Raw(text.data(), text.size());
        End(s);
    }
    void Clear() {
        used = 0;
    }
    std::size_t Size() const {
        return used;
    }
    std::size_t Capacity() const {
        return capacity;
    }
    u8* Data() {
        return base;
    }
    std::span<const u8> Bytes() const {
        return {base, used};
    }

private:
    u8* base = nullptr;
    std::size_t capacity = 0;
    std::size_t used = 0;
    const char* name = "";
};

struct EventRec {
    u32 time;
    u32 tid;
    u32 tag;
    u32 arg;
};

enum class Phase : u32 { Idle, Armed, Frame, Draining };

struct Capture {
    Memory::MemorySystem* memory{};
    std::thread thread;
    /// The capture thread runs (detached: joinable() says nothing once it is).
    std::atomic<bool> started{false};
    std::atomic<bool> capturing{false};

    // Events: a fixed ring written lock-free by any thread while capturing.
    static constexpr std::size_t EventCapacity = 1u << 18;
    std::vector<EventRec> events = std::vector<EventRec>(EventCapacity);
    std::atomic<std::size_t> event_count{0};
    std::mutex tag_mutex;
    std::unordered_map<const char*, u32> tag_ids;
    std::vector<std::string> tag_names;
    std::mutex thread_mutex;
    std::unordered_map<u32, std::string> thread_names;
    std::mutex info_mutex;
    std::string info;

    // The frame: records in the order they happened, blobs of guest memory once each.
    std::atomic<Phase> phase{Phase::Idle};
    std::mutex frame_mutex;
    Writer head; // WriteFile's records before the frame
    Writer frame;
    Writer blobs;
    SceUID block{-1};
    std::unordered_map<u64, u32> blob_ids;
    // Set once the blobs reach FrameBlobBudget: later ranges are recorded without their bytes.
    bool blobs_full{};
    u32 next_op{1};
    u32 first_op{}, last_op{};
    std::atomic<u32> ended_op{0};
    u32 frame_number{};
    // Vblanks the recorded frame still spans (vprof_vblanks).
    u32 frame_vblanks{};
    // Render thread: the op running, 0 outside the frame's ops.
    std::atomic<u32> current_op{0};
    std::vector<u32> last_regs;
    std::array<std::array<float, 4>, 96> last_vs_f{};
    std::array<std::array<float, 4>, 96> last_gs_f{};
    u64 last_vs_code{}, last_gs_code{};
    u32 last_pass = ~0u;
    // The frame's results (PRES), numbered from 0.
    u32 result_index{};
    // ProfileStart/ProfileStop (the replay's `profile`): a capture of the events on request,
    // whatever the flags say, ended by the request; `written` once its file is.
    std::atomic<bool> on_demand{false};
    std::atomic<bool> start_now{false};
    std::atomic<bool> stop_now{false};
    std::atomic<u32> written{0};
    /// `written` when ProfileStart asked for its capture.
    std::atomic<u32> profile_written{0};
};

Capture& S() {
    static Capture s;
    return s;
}

u32 TagId(const char* tag) {
    Capture& s = S();
    std::scoped_lock lock{s.tag_mutex};
    const auto it = s.tag_ids.find(tag);
    if (it != s.tag_ids.end()) {
        return it->second;
    }
    const u32 id = static_cast<u32>(s.tag_names.size());
    s.tag_ids.emplace(tag, id);
    s.tag_names.emplace_back(tag ? tag : "?");
    return id;
}

u32 ThisThread() {
    const u32 tid = static_cast<u32>(sceKernelGetThreadId());
    Capture& s = S();
    std::scoped_lock lock{s.thread_mutex};
    if (!s.thread_names.contains(tid)) {
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        std::string name = "?";
        if (sceKernelGetThreadInfo(static_cast<SceUID>(tid), &info) >= 0) {
            name = info.name;
        }
        s.thread_names.emplace(tid, std::move(name));
    }
    return tid;
}

// The PICA state record (FORMAT.md "PSTA").
void WriteState(Writer& w, const Pica::PicaCore& pica) {
    const std::size_t r = w.Begin(Tag("PSTA"));
    w.Word(static_cast<u32>(pica.regs.reg_array.size()));
    w.Raw(pica.regs.reg_array.data(), pica.regs.reg_array.size() * 4);
    const u32 lcd = static_cast<u32>(Pica::RegsLcd::NumIds());
    w.Word(lcd);
    for (u32 i = 0; i < lcd; i++) {
        w.Word(pica.regs_lcd[i]);
    }
    for (const Pica::ShaderSetup* setup : {&pica.vs_setup, &pica.gs_setup}) {
        w.Raw(setup->GetProgramCode().data(), setup->GetProgramCode().size() * 4);
        w.Raw(setup->GetSwizzleData().data(), setup->GetSwizzleData().size() * 4);
        for (const auto& v : setup->uniforms.f) {
            for (u32 c = 0; c < 4; c++) {
                w.Float(v[c].ToFloat32());
            }
        }
        u32 b = 0;
        for (u32 i = 0; i < 16; i++) {
            b |= setup->uniforms.b[i] ? 1u << i : 0;
        }
        w.Word(b);
        for (const auto& v : setup->uniforms.i) {
            w.Word(u32(v.x) | u32(v.y) << 8 | u32(v.z) << 16 | u32(v.w) << 24);
        }
        w.Word(setup->entry_point);
    }
    for (const auto& v : pica.input_default_attributes) {
        for (u32 c = 0; c < 4; c++) {
            w.Float(v[c].ToFloat32());
        }
    }
    const auto& pt = pica.proctex;
    w.Raw(pt.noise_table.data(), sizeof(pt.noise_table));
    w.Raw(pt.color_map_table.data(), sizeof(pt.color_map_table));
    w.Raw(pt.alpha_map_table.data(), sizeof(pt.alpha_map_table));
    w.Raw(pt.color_table.data(), sizeof(pt.color_table));
    w.Raw(pt.color_diff_table.data(), sizeof(pt.color_diff_table));
    w.Raw(pica.lighting.luts.data(), sizeof(pica.lighting.luts));
    w.Raw(pica.fog.lut.data(), sizeof(pica.fog.lut));
    w.End(r);
}

void WriteFile(Capture& s, u32 start) {
    static u32 file_number = 0;
    // The records before the frame; the frame and its blobs follow them into the compressor as they
    // are, never copied (a copy beside them runs the heap out).
    Writer& w = s.head;
    w.Clear();
    w.Word(Tag("VPRF"));
    w.Word(FileVersion);
    {
        std::string info;
        {
            std::scoped_lock lock{s.info_mutex};
            info = s.info;
        }
        info += fmt::format("start={}\n", start);
        w.Text(Tag("INFO"), 0, info);
    }
    {
        std::scoped_lock lock{s.thread_mutex};
        for (const auto& [tid, name] : s.thread_names) {
            w.Text(Tag("THRD"), tid, name);
        }
    }
    {
        std::scoped_lock lock{s.tag_mutex};
        for (u32 i = 0; i < s.tag_names.size(); i++) {
            w.Text(Tag("TAGN"), i, s.tag_names[i]);
        }
    }
    {
        const std::size_t n = std::min(s.event_count.load(), Capture::EventCapacity);
        const std::size_t r = w.Begin(Tag("EVTS"));
        w.Raw(s.events.data(), n * sizeof(EventRec));
        w.End(r);
    }
    const std::string path = fmt::format("host0:vprof-{}.vprof", file_number++);
    const u64 t0 = sceKernelGetProcessTimeWide();
    std::scoped_lock lock{s.frame_mutex};
    const std::size_t total = w.Size() + s.frame.Size() + s.blobs.Size();
    std::size_t packed = 0;
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        const u32 head[3] = {Tag("VPRZ"), FileVersion, static_cast<u32>(total)};
        std::fwrite(head, 4, 3, f);
        Common::Compression::ZstdStreamWriter z{total, [f, &packed](std::span<const u8> bytes) {
                                                    std::fwrite(bytes.data(), 1, bytes.size(), f);
                                                    packed += bytes.size();
                                                }};
        z.Write(w.Bytes());
        z.Write(s.frame.Bytes());
        z.Write(s.blobs.Bytes());
        z.Finish();
        std::fclose(f);
    }
    LOG_INFO(Render, "vprof: {} written: {} events, frame {} KiB, {} KiB packed, {:.1f} s to write",
             path, std::min(s.event_count.load(), Capture::EventCapacity),
             (s.frame.Size() + s.blobs.Size()) >> 10, packed >> 10,
             (sceKernelGetProcessTimeWide() - t0) / 1e6);
}

bool Triggered() {
    if (std::FILE* f = std::fopen("host0:vprof-trigger", "rb")) {
        std::fclose(f);
        std::remove("host0:vprof-trigger");
        return true;
    }
    return false;
}

void Run() {
    Capture& s = S();
    const u32 at_s = SgxdFlagValue("vprof_at") ? SgxdFlagValue("vprof_at") : 10;
    const u32 len_s = SgxdFlagValue("vprof_len") ? SgxdFlagValue("vprof_len") : 2;
    const bool trigger = SgxdFlag("vprof_trigger");
    const u64 begin = sceKernelGetProcessTimeWide();
    bool first = true;
    for (;;) {
        // Wait for the start: the time, or the trigger file.
        for (u32 tick = 0;; tick++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (first && !trigger && !s.on_demand &&
                sceKernelGetProcessTimeWide() - begin >= at_s * 1000000ull) {
                break;
            }
            if (s.start_now.exchange(false)) {
                break;
            }
            if (trigger && tick % 10 == 9 && Triggered()) {
                break;
            }
        }
        first = false;
        // On request, until ProfileStop or for vprof_len seconds (10 by default).
        const u32 limit_s = s.on_demand && !SgxdFlagValue("vprof_len") ? 10 : len_s;
        s.stop_now = false;
        LOG_INFO(Render, "vprof: capture starts ({} s)", limit_s);
        const u32 start = Now();
        s.event_count = 0;
        {
            std::scoped_lock lock{s.frame_mutex};
            if (s.block < 0) {
                // The capture's block, at the first capture: only a run that captures has it.
                const std::size_t size = HeadRegion + FrameRegion + BlobRegion;
                s.block = sceKernelAllocMemBlock("azahar-vprof", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                                 static_cast<SceSize>(size), nullptr);
                ASSERT_MSG(s.block >= 0, "vprof: no {} MiB of main memory for the capture: {:#x}",
                           size >> 20, static_cast<u32>(s.block));
                void* base = nullptr;
                const int got = sceKernelGetMemBlockBase(s.block, &base);
                ASSERT_MSG(got >= 0, "vprof: sceKernelGetMemBlockBase {:#x}",
                           static_cast<u32>(got));
                u8* const p = static_cast<u8*>(base);
                s.head.Attach(p, HeadRegion, "file");
                s.frame.Attach(p + HeadRegion, FrameRegion, "frame");
                s.blobs.Attach(p + HeadRegion + FrameRegion, BlobRegion, "blob");
            }
            s.frame.Clear();
            s.blobs.Clear();
            s.blob_ids.clear();
            s.blobs_full = false;
            s.last_regs.clear();
            s.last_vs_f = {};
            s.last_gs_f = {};
            s.last_vs_code = s.last_gs_code = 0;
            s.last_pass = ~0u;
        }
        s.capturing = true;
        // A capture on request has no frame of its own: the replay is the frame.
        s.phase = s.on_demand ? Phase::Idle : Phase::Armed;
        while (Now() - start < limit_s * 1000000u && !s.stop_now) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        s.capturing = false;
        // The frame's ops finish on the async render chain (a second at most; a frame that never
        // started or ended is written as far as it got).
        for (u32 i = 0; i < 100 && s.phase != Phase::Idle; i++) {
            if (s.phase == Phase::Draining && s.ended_op.load() >= s.last_op) {
                s.phase = Phase::Idle;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (s.phase != Phase::Idle) {
            LOG_WARNING(Render, "vprof: the frame did not complete (phase {})",
                        static_cast<u32>(s.phase.load()));
            s.phase = Phase::Idle;
        }
        s.current_op = 0;
        WriteFile(s, start);
        s.written++;
        if (!trigger && !s.on_demand) {
            return;
        }
    }
}

void Record(u32 tag, std::initializer_list<u32> words) {
    Capture& s = S();
    const std::size_t r = s.frame.Begin(tag);
    for (u32 w : words) {
        s.frame.Word(w);
    }
    s.frame.End(r);
}

u32 Blob(Capture& s, const u8* data, u32 size) {
    const u64 hash = Common::ComputeHash64(data, size) ^ size;
    const auto it = s.blob_ids.find(hash);
    if (it != s.blob_ids.end()) {
        return it->second;
    }
    // A frame that reads more than the region keeps its records, not the bytes.
    if (s.blobs.Size() + size + 12 > s.blobs.Capacity()) {
        if (!s.blobs_full) {
            s.blobs_full = true;
            LOG_WARNING(Render,
                        "vprof: the frame's memory reached {} MiB: later ranges have no "
                        "bytes, so the frame cannot be replayed",
                        s.blobs.Capacity() >> 20);
        }
        return NoBlob;
    }
    const u32 id = static_cast<u32>(s.blob_ids.size());
    s.blob_ids.emplace(hash, id);
    const std::size_t r = s.blobs.Begin(Tag("BLOB"));
    s.blobs.Word(id);
    s.blobs.Raw(data, size);
    s.blobs.End(r);
    return id;
}

} // Anonymous namespace

void Start(Memory::MemorySystem& memory) {
    if (!SgxdFlag("vprof")) {
        return;
    }
    Capture& s = S();
    // The memory of each renderer made: a loaded state makes the core's memory again.
    s.memory = &memory;
    // Once: a renderer made again (a loaded state) must not arm a second capture thread.
    if (s.started.exchange(true)) {
        return;
    }
    s.thread = std::thread(Run);
    s.thread.detach();
    LOG_INFO(Render, "vprof: armed");
}

bool ProfileStart(Memory::MemorySystem& memory) {
    Capture& s = S();
    s.memory = &memory;
    s.on_demand = true;
    if (!s.started.exchange(true)) {
        s.thread = std::thread(Run);
        s.thread.detach();
    }
    s.profile_written = s.written.load();
    s.start_now = true;
    // The capture thread polls every 100 ms: the request's work starts once it captures.
    for (u32 i = 0; i < 300 && !s.capturing; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return s.capturing;
}

void ProfileStop() {
    Capture& s = S();
    // The capture may have reached its limit already; its file is waited for either way.
    s.stop_now = true;
    // The file goes to the host PC at its pace (a few MiB at a few hundred KiB/s).
    for (u32 i = 0; i < 12000 && s.written == s.profile_written; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Event(const char* tag, u32 arg) {
    Capture& s = S();
    if (!s.capturing.load(std::memory_order_relaxed)) {
        return;
    }
    const std::size_t i = s.event_count.fetch_add(1, std::memory_order_relaxed);
    if (i < Capture::EventCapacity) {
        s.events[i] = {Now(), ThisThread(), TagId(tag), arg};
    }
}

bool Recording() {
    return S().phase.load(std::memory_order_relaxed) == Phase::Frame;
}

bool Capturing() {
    const auto& s = S();
    return s.capturing.load(std::memory_order_relaxed) ||
           s.phase.load(std::memory_order_relaxed) != Phase::Idle;
}

void Info(const char* key, const char* value) {
    Capture& s = S();
    std::scoped_lock lock{s.info_mutex};
    s.info += fmt::format("{}={}\n", key, value);
}

bool VBlank() {
    Capture& s = S();
    const Phase p = s.phase.load();
    if (p == Phase::Armed && s.capturing) {
        std::scoped_lock lock{s.frame_mutex};
        s.frame_number++;
        s.first_op = s.next_op;
        s.result_index = 0;
        Record(Tag("FRAM"), {s.frame_number, Now()});
        // A title running below full speed spreads a frame over several vblanks: vprof_vblanks=N
        // records N vblank intervals as the frame (1 by default).
        s.frame_vblanks = std::max(1u, SgxdFlagValue("vprof_vblanks"));
        s.phase = Phase::Frame;
        return true;
    }
    if (p == Phase::Frame && --s.frame_vblanks == 0) {
        std::scoped_lock lock{s.frame_mutex};
        s.last_op = s.next_op - 1;
        Record(Tag("VBLK"), {s.frame_number, Now()});
        s.phase = Phase::Draining;
    }
    return false;
}

void State(const Pica::PicaCore& pica) {
    struct Capture& s = S();
    std::scoped_lock lock{s.frame_mutex};
    WriteState(s.frame, pica);
    s.last_regs.assign(pica.regs.reg_array.begin(), pica.regs.reg_array.end());
    for (u32 i = 0; i < 96; i++) {
        for (u32 c = 0; c < 4; c++) {
            s.last_vs_f[i][c] = pica.vs_setup.uniforms.f[i][c].ToFloat32();
            s.last_gs_f[i][c] = pica.gs_setup.uniforms.f[i][c].ToFloat32();
        }
    }
    s.last_vs_code = const_cast<Pica::ShaderSetup&>(pica.vs_setup).GetProgramCodeHash() ^
                     const_cast<Pica::ShaderSetup&>(pica.vs_setup).GetSwizzleDataHash();
    s.last_gs_code = const_cast<Pica::ShaderSetup&>(pica.gs_setup).GetProgramCodeHash() ^
                     const_cast<Pica::ShaderSetup&>(pica.gs_setup).GetSwizzleDataHash();
}

void RegWrite(u32 index, u32 value, bool lcd) {
    if (!Recording()) {
        return;
    }
    Capture& s = S();
    std::scoped_lock lock{s.frame_mutex};
    Record(Tag("EREG"), {index, value, lcd ? 1u : 0u, Now()});
}

void Gsp(const u32* words) {
    if (!Recording()) {
        return;
    }
    Capture& s = S();
    std::scoped_lock lock{s.frame_mutex};
    const std::size_t r = s.frame.Begin(Tag("GSPC"));
    s.frame.Raw(words, 32);
    s.frame.Word(Now());
    s.frame.End(r);
}

void Dma(const u32* triples, u32 runs) {
    if (!Recording()) {
        return;
    }
    Capture& s = S();
    std::scoped_lock lock{s.frame_mutex};
    const std::size_t r = s.frame.Begin(Tag("EDMA"));
    s.frame.Word(Now());
    s.frame.Word(runs);
    s.frame.Raw(triples, runs * 12);
    s.frame.End(r);
}

u32 OpQueued(u32 kind) {
    if (ReplayActive()) {
        return ReplayOpQueued();
    }
    if (!Recording()) {
        return 0;
    }
    Capture& s = S();
    std::scoped_lock lock{s.frame_mutex};
    const u32 seq = s.next_op++;
    Record(Tag("OPQ "), {seq, kind, Now()});
    return seq;
}

bool InOp() {
    return S().current_op.load(std::memory_order_relaxed) != 0;
}

void OpBegin(u32 seq) {
    if (seq == 0) {
        return;
    }
    if (ReplayActive()) {
        ReplayOpBegin(seq);
        return;
    }
    Capture& s = S();
    s.current_op = seq;
    std::scoped_lock lock{s.frame_mutex};
    Record(Tag("OPB "), {seq, Now()});
}

void OpEnd(u32 seq) {
    if (seq == 0) {
        return;
    }
    if (ReplayActive()) {
        ReplayOpEnd(seq);
        return;
    }
    Capture& s = S();
    {
        std::scoped_lock lock{s.frame_mutex};
        Record(Tag("OPE "), {seq, Now()});
    }
    s.current_op = 0;
    s.ended_op = seq;
}

void Memory(Mem kind, u32 paddr, u32 size) {
    Capture& s = S();
    const u32 op = s.current_op.load(std::memory_order_relaxed);
    if (op == 0 || size == 0 || !s.memory) {
        return;
    }
    const u8* data = s.memory->GetPhysicalPointer(paddr);
    if (!data) {
        return;
    }
    std::scoped_lock lock{s.frame_mutex};
    const u32 blob = Blob(s, data, size);
    Record(Tag("MEML"), {op, static_cast<u32>(kind), paddr, size, blob});
}

void Draw(const Pica::PicaCore& pica, u32 pass, u32 draw, u32 vertices) {
    Capture& s = S();
    const u32 op = s.current_op.load(std::memory_order_relaxed);
    if (op == 0) {
        return;
    }
    std::scoped_lock lock{s.frame_mutex};
    Writer& w = s.frame;
    // A program the frame has not had yet goes in before the draw that uses it.
    const auto program = [&](u32 which, const Pica::ShaderSetup& setup, u64& last) {
        auto& m = const_cast<Pica::ShaderSetup&>(setup);
        const u64 h = m.GetProgramCodeHash() ^ m.GetSwizzleDataHash();
        if (h == last) {
            return;
        }
        last = h;
        const std::size_t r = w.Begin(Tag("PROG"));
        w.Word(which);
        w.Raw(setup.GetProgramCode().data(), setup.GetProgramCode().size() * 4);
        w.Raw(setup.GetSwizzleData().data(), setup.GetSwizzleData().size() * 4);
        w.End(r);
    };
    program(0, pica.vs_setup, s.last_vs_code);
    program(1, pica.gs_setup, s.last_gs_code);
    const std::size_t r = w.Begin(Tag("DRAW"));
    w.Word(op);
    w.Word(pass);
    w.Word(draw);
    w.Word(vertices);
    w.Word(Now());
    // The registers that changed since the last draw (or the frame's state).
    const auto& regs = pica.regs.reg_array;
    if (s.last_regs.size() != regs.size()) {
        s.last_regs.assign(regs.size(), 0);
    }
    const std::size_t count_at = w.Size();
    w.Word(0);
    u32 n = 0;
    for (u32 i = 0; i < regs.size(); i++) {
        if (regs[i] != s.last_regs[i]) {
            s.last_regs[i] = regs[i];
            w.Word(i);
            w.Word(regs[i]);
            n++;
        }
    }
    std::memcpy(w.Data() + count_at, &n, 4);
    const auto uniforms = [&](const Pica::ShaderSetup& setup,
                              std::array<std::array<float, 4>, 96>& last) {
        const std::size_t at = w.Size();
        w.Word(0);
        u32 changed = 0;
        for (u32 i = 0; i < 96; i++) {
            std::array<float, 4> v;
            for (u32 c = 0; c < 4; c++) {
                v[c] = setup.uniforms.f[i][c].ToFloat32();
            }
            if (std::memcmp(v.data(), last[i].data(), 16) != 0) {
                last[i] = v;
                w.Word(i);
                for (float f : v) {
                    w.Float(f);
                }
                changed++;
            }
        }
        std::memcpy(w.Data() + at, &changed, 4);
        u32 b = 0;
        for (u32 i = 0; i < 16; i++) {
            b |= setup.uniforms.b[i] ? 1u << i : 0;
        }
        w.Word(b);
        for (const auto& v : setup.uniforms.i) {
            w.Word(u32(v.x) | u32(v.y) << 8 | u32(v.z) << 16 | u32(v.w) << 24);
        }
    };
    uniforms(pica.vs_setup, s.last_vs_f);
    uniforms(pica.gs_setup, s.last_gs_f);
    w.End(r);
    (void)s.last_pass;
}

void Pass(u32 pass, u32 target, u32 width, u32 height, u32 draws, bool sync) {
    Capture& s = S();
    if (!s.capturing.load(std::memory_order_relaxed)) {
        return;
    }
    Event("pass", pass);
    if (s.current_op.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::scoped_lock lock{s.frame_mutex};
    Record(Tag("PASS"), {s.current_op.load(), pass, target, width | height << 16, draws,
                         sync ? 1u : 0u, Now()});
}

void PassTimes(u32 pass, u32 ta_us, u32 d3_us) {
    Capture& s = S();
    if (s.current_op.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::scoped_lock lock{s.frame_mutex};
    Record(Tag("PTIM"), {s.current_op.load(), pass, ta_us, d3_us});
}

bool FrameSync() {
    return Recording();
}

void FrameRecord(u32 tag, const std::vector<u32>& words) {
    Capture& s = S();
    std::scoped_lock lock{s.frame_mutex};
    const std::size_t r = s.frame.Begin(tag);
    for (u32 w : words) {
        s.frame.Word(w);
    }
    s.frame.End(r);
}

u32 FrameBlob(const u8* data, u32 size) {
    Capture& s = S();
    std::scoped_lock lock{s.frame_mutex};
    return Blob(s, data, size);
}

bool WantResults() {
    return ReplayActive() ? ReplayWantsResults() : Recording() && InOp();
}

void FrameResult(u32 kind, u32 addr, u32 width, u32 height, u32 format, const u8* data,
                 u32 size) {
    if (ReplayActive()) {
        ReplayResult(kind, addr, width, height, format, data, size);
        return;
    }
    if (!Recording() || !InOp()) {
        return;
    }
    Capture& s = S();
    std::scoped_lock lock{s.frame_mutex};
    const u32 blob = Blob(s, data, size);
    Record(Tag("PRES"), {s.current_op.load(), s.result_index++, kind, addr, width, height,
                         format, blob});
}

} // namespace SgxdRenderer::Vprof

#endif
