// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bit>
#include <cstring>
#include <thread>
#include <type_traits>
#include "common/assert.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "core/memory.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"
#include "video_core/renderer_sgxd/render_thread.h"
#include "video_core/renderer_sgxd/renderer_sgxd.h"

#include "sgxd.h"
#include "sgxd_mkq.h"
#include "sgxd_ptla.h"
#include "sgxd_session.h"

namespace SgxdRenderer {

enum class RenderThread::Op : u16 {
    /// The rest of the ring is unused: the next record is at its start.
    Wrap,
    // The PICA state, applied to the render side's PicaCore.
    Regs,     ///< the dirty table's 12 words, then each dirty register's value, in order
    Uniforms, ///< extra: the setup (0 VS, 1 GS); UniformsHead, then the values it names
    Code,     ///< extra: the setup; RangeHead, then the words
    Swizzle,  ///< extra: the setup; RangeHead, then the words
    Lighting, ///< the changed tables' mask, then each one's 256 words, in order
    Fog,      ///< the 128 words
    ProcTex,  ///< the dirty mask, then the five tables
    DefaultAttributes,
    // The rasterizer's calls.
    Draw,      ///< extra: indexed
    GsIndices, ///< extra: 16-bit; a geometry draw's indices as the guest had them at the draw
    MergedDraw, ///< MergedDrawHead, then its vertices' numbers (u16): RenderThread::MergeDraw
    ImmediateVertex,
    FlushImmediate,
    AddTriangle,
    DrawTriangles,
    P3D,
    Fill,
    DisplayTransfer,
    TextureCopy,
    Dma,
    GpuIdle,
    Fence,
    Present,
    Call,
    Check, ///< rendercheck: CheckRecord
};

struct RenderThread::Head {
    Op op;
    u16 extra;
    u32 bytes; ///< the whole record's, this header included: a multiple of 8
};

namespace {

constexpr u32 Align8(u32 n) {
    return (n + 7) & ~7u;
}

struct UniformsHead {
    u32 entry_point;
    std::array<u32, 3> f; ///< the float uniforms that follow, by index
    u32 bool_int;         ///< the bool and int uniforms follow the floats
};

struct RangeHead {
    u32 lo;
    u32 count;
    u32 biggest;
};

struct DmaRecord {
    PAddr src;
    PAddr dst;
    u32 size;
    u64 src_hash; ///< rendercheck: the source's bytes as the DMA was asked for
};

struct TriangleRecord {
    std::array<Pica::OutputVertex, 3> v;
};

struct PresentRecord {
    std::array<Pica::FramebufferConfig, 2> fb;
    std::array<Pica::ColorFill, 2> fill;
};

struct CallRecord {
    std::function<void()> fn;
    bool sync;
    std::atomic<bool> finished{};
};

/// gxm_flags `rendercheck`: the emulation side's PICA state at a draw.
struct CheckRecord {
    std::array<u32, Pica::RegsInternal::NUM_REGS> regs;
    std::array<Pica::Uniforms, 2> uniforms;
    std::array<u64, 2> code_hash;
    std::array<u64, 2> swizzle_hash;
    Pica::AttributeBuffer defaults;
    std::array<std::array<u32, 256>, 24> lighting;
    std::array<u32, 128> fog;
    std::array<u32, 128 * 3 + 256 * 2> proctex;
};

static_assert(std::is_trivially_copyable_v<CheckRecord>);
static_assert(std::is_trivially_copyable_v<Pica::OutputVertex>);
static_assert(std::is_trivially_copyable_v<Pica::AttributeBuffer>);
static_assert(std::is_trivially_copyable_v<Pica::MemoryFillConfig>);
static_assert(std::is_trivially_copyable_v<Pica::DisplayTransferConfig>);
static_assert(std::is_trivially_copyable_v<PresentRecord>);
static_assert(std::is_trivially_copyable_v<Pica::Uniforms>);

constexpr u32 LightingTableBytes = 256 * sizeof(u32);

using Clock = std::chrono::steady_clock;

#ifdef VITA_DIAGNOSTICS
u64 Since(Clock::time_point start) {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

bool LatelogNow() {
    static const bool on = SgxdFlag("latelog") || SgxdFlagValue("latelog_at") != 0;
    static const u64 from_us = u64{SgxdFlagValue("latelog_at")} * 1000000;
    return on && static_cast<u64>(Clock::now().time_since_epoch().count()) >= from_us;
}

/// sgxd_flags `holdlog`: each FCRAM page's last held index list (address, bytes, bit 31 16-bit, bit
/// 30 a geometry draw), for the busy-page log.
constexpr u32 NotePages = (Memory::FCRAM_PADDR_END - Memory::FCRAM_PADDR) >> 12;
std::array<std::atomic<u32>, NotePages> index_note_addr{};
std::array<std::atomic<u32>, NotePages> index_note_info{};

void NoteIndexHold(PAddr address, u32 bytes, bool u16, bool gs) {
    static const bool holdlog = SgxdFlag("holdlog") || SgxdFlagValue("holdlog") != 0;
    if (!holdlog || bytes == 0 || address < Memory::FCRAM_PADDR ||
        address + bytes > Memory::FCRAM_PADDR_END) {
        return;
    }
    const u32 info =
        std::min(bytes, 0x3fffffffu) | (u16 ? 0x80000000u : 0u) | (gs ? 0x40000000u : 0u);
    const u32 last = (address + bytes - 1 - Memory::FCRAM_PADDR) >> 12;
    for (u32 p = (address - Memory::FCRAM_PADDR) >> 12; p <= last; p++) {
        index_note_addr[p].store(address, std::memory_order_relaxed);
        index_note_info[p].store(info, std::memory_order_relaxed);
    }
}

std::string DescribeIndexHold(PAddr page_addr) {
    if (page_addr < Memory::FCRAM_PADDR || page_addr >= Memory::FCRAM_PADDR_END) {
        return "not FCRAM";
    }
    const u32 p = (page_addr - Memory::FCRAM_PADDR) >> 12;
    const u32 info = index_note_info[p].load(std::memory_order_relaxed);
    if (info == 0) {
        return "no index list held here";
    }
    return fmt::format("last index list {:#010x}+{:#x}, {}-bit{}",
                       index_note_addr[p].load(std::memory_order_relaxed), info & 0x3fffffffu,
                       (info & 0x80000000u) != 0 ? 16 : 8,
                       (info & 0x40000000u) != 0 ? ", geometry shader" : "");
}
#endif

} // namespace

RenderThread::RenderThread(Pica::PicaCore& pica_, Pica::PicaCore& render_pica_,
                           RendererGxm& renderer_, RasterizerGxm& rasterizer_,
                           Memory::MemorySystem& memory_)
    : pica{pica_}, render_pica{render_pica_}, renderer{renderer_}, rasterizer{rasterizer_},
      memory{memory_}, gpu_lock{gpu_lock_}, ring{std::make_unique<u8[]>(RingSize)} {
    static_assert(std::has_single_bit(RingSize), "positions wrap at 2^32");
    static_assert(sizeof(Head) == 8, "records stay 8-byte aligned");
    // The render side's state starts as the emulation side's: the first records carry all of
    // it (the dirty table and the LUT marks start full, and PicaCore::MarkAllChanged covers the
    // rest).
    pica.MarkAllChanged();
    constexpr std::size_t StackSize = 2 * 1024 * 1024;
    thread = Common::NamedThread{Common::ThreadCfg{"render", StackSize}, [this] { Loop(); }};
    LOG_INFO(Render, "render thread: on Vita core 0, fences polled every {} us while pending",
             poll_interval.count());
    check = GxmFlag("rendercheck");
}
#endif

void RenderThread::Watch(std::stop_token stop) {
    Common::SetCurrentThreadRole(Common::ThreadRole::Other);
    u32 last = records_run.load();
    while (!stop.stop_requested()) {
        for (int i = 0; i < 20 && !stop.stop_requested(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const u32 now = records_run.load();
        const Wait wait = emu_wait.load();
        const u32 queued = published.load() - read.load();
        if (now == last && (wait != Wait::None || queued != 0)) {
            const u64 fence = front_fence.load();
            const volatile SgxdEvents* const ev = sgxd_ptla_page();
            LOG_WARNING(Render,
                        "renderwatch: no record run in 2 s; running op {:#x}, {} bytes queued, "
                        "emulation side waits {} (ticket {}), tickets {} reached {}, parked {}, "
                        "pending fences {}, the oldest's render {} ptla {}; sgxd: lists sent {} "
                        "done {} (TA {}), pending 3D {}, PTLA sent {} done {}, fault {}, TA irqs "
                        "{}, 3D irqs {}",
                        running_op.load(), queued, static_cast<u32>(wait), waited_ticket.load(),
                        tickets, reached.load(), parked.load(), pending.size(),
                        static_cast<u32>(fence >> 32), static_cast<u32>(fence), sgxd_chain_fence(),
                        ev ? ev->chains : 0u, ev ? ev->chain_ta : 0u,
                        sgxd_session_pending() != nullptr ? 1 : 0, sgxd_ptla_fence(),
                        ev ? ev->twod : 0u, ev ? ev->chain_fault : 0u, ev ? ev->ta : 0u,
                        ev ? ev->d3 : 0u);
        }
        last = now;
    }
}

RenderThread::~RenderThread() {
    {
        while (hold_depth != 0) {
            Release();
        }
    }
    {
        std::scoped_lock lk{work};
        stopping = true;
        work.NotifyAll();
    }
    thread.join();
}

void RenderThread::SyncState() {
    // Registers: each one the command lists or the GPU's own writes marked since the last call.
    auto& dirty = pica.dirty_regs;
    u32 count = 0;
    for (const u64 q : dirty.qwords) {
        count += static_cast<u32>(std::popcount(q));
    }
    if (count != 0) {
        u8* p = Begin(Op::Regs, sizeof(dirty.qwords) + count * sizeof(u32));
        std::memcpy(p, dirty.qwords.data(), sizeof(dirty.qwords));
        u32* values = reinterpret_cast<u32*>(p + sizeof(dirty.qwords));
        // In 32-bit words: ARMv7 finds a word's lowest bit in two instructions.
        const u32* const regs = pica.regs.internal.reg_array.data();
        u32 halves[sizeof(dirty.qwords) / sizeof(u32)];
        std::memcpy(halves, dirty.qwords.data(), sizeof(halves));
        for (u32 w = 0; w < std::size(halves); w++) {
            for (u32 h = halves[w]; h != 0; h &= h - 1) {
                *values++ = regs[w * 32 + static_cast<u32>(std::countr_zero(h))];
            }
        }
        Commit();
        dirty.Reset();
    }

    for (u16 unit = 0; unit < 2; unit++) {
        Pica::ShaderSetup& setup = unit == 0 ? pica.vs_setup : pica.gs_setup;
        if (setup.uniforms_dirty) {
            UniformsHead h{setup.entry_point, setup.f_changed, setup.bi_changed};
            // Dirty with nothing marked (assigned directly, vprof's replay): all of them.
            if ((h.f[0] | h.f[1] | h.f[2]) == 0 && !setup.bi_changed) {
                h.f = {~0u, ~0u, 0xffffffffu >> (96 - static_cast<u32>(setup.uniforms.f.size()))};
                h.bool_int = 1;
            }
            const u32 floats = static_cast<u32>(std::popcount(h.f[0]) + std::popcount(h.f[1]) +
                                                std::popcount(h.f[2]));
            const u32 tail = h.bool_int ? sizeof(setup.uniforms.b) + sizeof(setup.uniforms.i) : 0;
            u8* p = Begin(Op::Uniforms,
                          sizeof(h) + floats * sizeof(setup.uniforms.f[0]) + tail, unit);
            std::memcpy(p, &h, sizeof(h));
            p += sizeof(h);
            for (u32 w = 0; w < 3; w++) {
                for (u32 bits = h.f[w]; bits != 0; bits &= bits - 1) {
                    const u32 index = w * 32 + static_cast<u32>(std::countr_zero(bits));
                    std::memcpy(p, &setup.uniforms.f[index], sizeof(setup.uniforms.f[0]));
                    p += sizeof(setup.uniforms.f[0]);
                }
            }
            if (h.bool_int) {
                std::memcpy(p, setup.uniforms.b.data(), sizeof(setup.uniforms.b));
                std::memcpy(p + sizeof(setup.uniforms.b), setup.uniforms.i.data(),
                            sizeof(setup.uniforms.i));
            }
            Commit();
            setup.uniforms_dirty = false;
            setup.f_changed = {};
            setup.bi_changed = false;
        }
        if (setup.code_hi > setup.code_lo) {
            const RangeHead h{setup.code_lo, setup.code_hi - setup.code_lo,
                              setup.GetBiggestProgramSize()};
            u8* p = Begin(Op::Code, sizeof(h) + h.count * sizeof(u32), unit);
            std::memcpy(p, &h, sizeof(h));
            std::memcpy(p + sizeof(h), setup.GetProgramCode().data() + h.lo, h.count * sizeof(u32));
            Commit();
            setup.code_lo = Pica::MAX_PROGRAM_CODE_LENGTH;
            setup.code_hi = 0;
        }
        if (setup.swizzle_hi > setup.swizzle_lo) {
            const RangeHead h{setup.swizzle_lo, setup.swizzle_hi - setup.swizzle_lo,
                              setup.GetBiggestSwizzleSize()};
            u8* p = Begin(Op::Swizzle, sizeof(h) + h.count * sizeof(u32), unit);
            std::memcpy(p, &h, sizeof(h));
            std::memcpy(p + sizeof(h), setup.GetSwizzleData().data() + h.lo,
                        h.count * sizeof(u32));
            Commit();
            setup.swizzle_lo = Pica::MAX_SWIZZLE_DATA_LENGTH;
            setup.swizzle_hi = 0;
        }
    }

    if (const u32 mask = pica.lighting.lut_dirty & Pica::PicaCore::Lighting::LutAllDirty) {
        const u32 tables = static_cast<u32>(std::popcount(mask));
        u8* p = Begin(Op::Lighting, sizeof(u32) + tables * LightingTableBytes);
        std::memcpy(p, &mask, sizeof(mask));
        p += sizeof(mask);
        for (u32 bits = mask; bits != 0; bits &= bits - 1) {
            const u32 t = static_cast<u32>(std::countr_zero(bits));
            std::memcpy(p, pica.lighting.luts[t].data(), LightingTableBytes);
            p += LightingTableBytes;
        }
        Commit();
        pica.lighting.lut_dirty = 0;
    }
    if (pica.fog.lut_dirty) {
        u8* p = Begin(Op::Fog, sizeof(pica.fog.lut));
        std::memcpy(p, pica.fog.lut.data(), sizeof(pica.fog.lut));
        Commit();
        pica.fog.lut_dirty = false;
    }
    if (const u8 mask = pica.proctex.table_dirty) {
        const auto& t = pica.proctex;
        const u32 tables = sizeof(t.noise_table) + sizeof(t.color_map_table) +
                           sizeof(t.alpha_map_table) + sizeof(t.color_table) +
                           sizeof(t.color_diff_table);
        u8* p = Begin(Op::ProcTex, sizeof(u32) + tables);
        const u32 m = mask;
        std::memcpy(p, &m, sizeof(m));
        p += sizeof(m);
        for (const auto [data, bytes] :
             {std::pair<const void*, u32>{t.noise_table.data(), sizeof(t.noise_table)},
              {t.color_map_table.data(), sizeof(t.color_map_table)},
              {t.alpha_map_table.data(), sizeof(t.alpha_map_table)},
              {t.color_table.data(), sizeof(t.color_table)},
              {t.color_diff_table.data(), sizeof(t.color_diff_table)}}) {
            std::memcpy(p, data, bytes);
            p += bytes;
        }
        Commit();
        pica.proctex.table_dirty = 0;
    }
    if (pica.default_attributes_changed) {
        Put(Op::DefaultAttributes, pica.input_default_attributes);
        pica.default_attributes_changed = false;
    }
}

u8* RenderThread::Begin(Op op, u32 bytes, u16 extra) {
    const u32 need = static_cast<u32>(sizeof(Head)) + Align8(bytes);
    ASSERT_MSG(need <= RingSize / 4, "render thread: a {}-byte record", need);
    u32 at = write % RingSize;
    // A record never wraps: the rest of the ring goes to a Wrap record.
    const u32 skip = RingSize - at < need ? RingSize - at : 0;
    if (RingSize - (write - read.load(std::memory_order_acquire)) < skip + need) {
        // The thread a whole ring behind: wait for it (a hold runs each record at once, so its
        // ring is always empty), with the records closed so far published.
        ASSERT(hold_depth == 0);
        const TraceSpan span{"emu_space", "emu_space_end"};
        if (published.load(std::memory_order_relaxed) != write) {
            Publish();
        }
#ifdef VITA_DIAGNOSTICS
        const auto start = Clock::now();
#endif
        std::scoped_lock lk{done};
        done_waiters.fetch_add(1);
        space_waiting.store(true);
        while (RingSize - (write - read.load(std::memory_order_acquire)) < skip + need) {
            SleepDone(EmuSpace);
        }
        space_waiting.store(false);
        done_waiters.fetch_sub(1);
#ifdef VITA_DIAGNOSTICS
        CountEmuWait(EmuSpace, start);
#endif
    }
    if (skip != 0) {
        const Head wrap{Op::Wrap, 0, skip};
        std::memcpy(ring.get() + at, &wrap, sizeof(wrap));
        write += skip;
        at = 0;
    }
    const Head head{op, extra, need};
    std::memcpy(ring.get() + at, &head, sizeof(head));
    open_record = need;
    return ring.get() + at + sizeof(Head);
}

void RenderThread::Commit() {
    write += open_record;
    if (publish_together && hold_depth == 0) {
        return;
    }
    Publish();
}

void RenderThread::Publish() {
    // Sequentially consistent against the thread's `sleeping`: either it sees this record
    // before it sleeps, or this sees it sleeping and wakes it.
    published.store(write);
    if (hold_depth != 0) {
        RunAvailable();
        return;
    }
    if (sleeping.load()) {
        std::scoped_lock lk{work};
        work.NotifyAll();
    }
}

bool RenderThread::RunsDraw(bool is_indexed) const {
    // Point mode: each vertex is its own invocation, so a run's draws are one draw of their vertices.
    const auto& pipeline = parse_pica.regs.internal.pipeline;
    return !is_indexed && parse_gs_bytes == 0 &&
           pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes &&
           pipeline.gs_config.mode == Pica::PipelineRegs::GSMode::Point &&
           pipeline.num_vertices != 0 && pipeline.num_vertices <= MaxRunVertices &&
           pipeline.vertex_offset + pipeline.num_vertices <= 0x10000u;
}

bool RenderThread::MergeDraw(bool is_indexed) {
    if (held.draws == 0 || !RunsDraw(is_indexed)) {
        return false;
    }
    // Nothing else to send since the run's first draw: no uniform, program or table change, and every
    // register as that draw had it but the first vertex.
    const auto& p = parse_pica;
    for (const Pica::ShaderSetup* s : {&p.vs_setup, &p.gs_setup}) {
        if (s->uniforms_dirty || s->code_hi > s->code_lo || s->swizzle_hi > s->swizzle_lo) {
            return false;
        }
    }
    if ((p.lighting.lut_dirty & Pica::PicaCore::Lighting::LutAllDirty) != 0 || p.fog.lut_dirty ||
        p.proctex.table_dirty != 0 || p.default_attributes_changed) {
        return false;
    }
    constexpr u32 OffsetReg = PICA_REG_INDEX(pipeline.vertex_offset);
    const auto& regs = p.regs.internal.reg_array;
    const auto& dirty = p.dirty_regs.qwords;
    for (u32 w = 0; w < dirty.size(); w++) {
        for (u64 q = dirty[w]; q != 0; q &= q - 1) {
            const u32 r = w * 64 + static_cast<u32>(std::countr_zero(q));
            if (r != OffsetReg && regs[r] != held.regs[r]) {
                return false;
            }
        }
    }
    const auto& pipeline = p.regs.internal.pipeline;
    if (held.vertices.size() + pipeline.num_vertices > MaxRunVertices) {
        return false;
    }
    // Its register writes stay marked: they go with the next state, after the run's record.
    for (u32 v = 0; v < pipeline.num_vertices; v++) {
        held.vertices.push_back(static_cast<u16>(pipeline.vertex_offset + v));
    }
    held.draws++;
    return true;
}

void RenderThread::FlushRun() {
    if (held.draws == 0 || held_flushing) {
        return;
    }
    held_flushing = true;
    if (held.draws == 1) {
        ParsedPut(Op::Draw, held.first_indices, 0);
    } else {
        const u32 count = static_cast<u32>(held.vertices.size());
        u8* p = ParsedBegin(Op::MergedDraw, sizeof(u32) + count * sizeof(u16));
        std::memcpy(p, &count, sizeof(count));
        std::memcpy(p + sizeof(count), held.vertices.data(), count * sizeof(u16));
        ParsedCommit();
    }
    held.draws = 0;
    held_flushing = false;
    PublishParsed();
}

template <typename T>
void RenderThread::Put(Op op, const T& payload, u16 extra) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(Begin(op, sizeof(T), extra), &payload, sizeof(T));
    Commit();
}

void RenderThread::Put(Op op) {
    Begin(op, 0);
    Commit();
}

void RenderThread::PutCheck() {
    CheckRecord r;
    r.regs = pica.regs.internal.reg_array;
    r.uniforms = {pica.vs_setup.uniforms, pica.gs_setup.uniforms};
    r.code_hash = {pica.vs_setup.GetProgramCodeHash(), pica.gs_setup.GetProgramCodeHash()};
    r.swizzle_hash = {pica.vs_setup.GetSwizzleDataHash(), pica.gs_setup.GetSwizzleDataHash()};
    r.defaults = pica.input_default_attributes;
    std::memcpy(r.lighting.data(), pica.lighting.luts.data(), sizeof(r.lighting));
    std::memcpy(r.fog.data(), pica.fog.lut.data(), sizeof(r.fog));
    const auto& t = pica.proctex;
    u8* q = reinterpret_cast<u8*>(r.proctex.data());
    for (const auto [data, bytes] :
         {std::pair<const void*, u32>{t.noise_table.data(), sizeof(t.noise_table)},
          {t.color_map_table.data(), sizeof(t.color_map_table)},
          {t.alpha_map_table.data(), sizeof(t.alpha_map_table)},
          {t.color_table.data(), sizeof(t.color_table)},
          {t.color_diff_table.data(), sizeof(t.color_diff_table)}}) {
        std::memcpy(q, data, bytes);
        q += bytes;
    }
    Put(Op::Check, r);
}

void RenderThread::RunCheck(const u8* payload) {
    static CheckRecord r;
    std::memcpy(&r, payload, sizeof(r));
    // Each difference is logged once: registers by index, the rest by what it is.
    static std::array<bool, Pica::RegsInternal::NUM_REGS> reg_seen{};
    static std::array<std::array<bool, 96>, 2> f_seen{};
    static u32 other_reports = 0;
    static u32 checks = 0;
    checks++;
    const auto& regs = render_pica.regs.internal.reg_array;
    for (u32 i = 0; i < regs.size(); i++) {
        if (regs[i] != r.regs[i] && !reg_seen[i]) {
            reg_seen[i] = true;
            LOG_ERROR(Render, "rendercheck: draw check {}: register {:#05x} is {:#010x}, the "
                              "emulation side's {:#010x}",
                      checks, i, regs[i], r.regs[i]);
        }
    }
    const auto other = [&](const char* what, u32 unit) {
        if (other_reports++ < 64) {
            LOG_ERROR(Render, "rendercheck: draw check {}: {} of the {} differs", checks, what,
                      unit == 0 ? "VS" : "GS");
        }
    };
    for (u32 unit = 0; unit < 2; unit++) {
        Pica::ShaderSetup& setup = unit == 0 ? render_pica.vs_setup : render_pica.gs_setup;
        for (u32 i = 0; i < setup.uniforms.f.size(); i++) {
            if (setup.uniforms.f[i] != r.uniforms[unit].f[i] && !f_seen[unit][i]) {
                f_seen[unit][i] = true;
                LOG_ERROR(Render, "rendercheck: draw check {}: float uniform {} of the {} differs",
                          checks, i, unit == 0 ? "VS" : "GS");
            }
        }
        if (setup.uniforms.b != r.uniforms[unit].b) {
            other("the bool uniforms", unit);
        }
        if (setup.uniforms.i != r.uniforms[unit].i) {
            other("the int uniforms", unit);
        }
        if (setup.GetProgramCodeHash() != r.code_hash[unit]) {
            other("the program", unit);
        }
        if (setup.GetSwizzleDataHash() != r.swizzle_hash[unit]) {
            other("the swizzle data", unit);
        }
    }
    const auto table = [&](const char* what, const void* mine, const void* theirs, u32 bytes) {
        if (std::memcmp(mine, theirs, bytes) != 0 && other_reports++ < 64) {
            LOG_ERROR(Render, "rendercheck: draw check {}: {} differs", checks, what);
        }
    };
    table("the default attributes", render_pica.input_default_attributes.data(),
          r.defaults.data(), sizeof(r.defaults));
    table("the lighting LUTs", render_pica.lighting.luts.data(), r.lighting.data(),
          sizeof(r.lighting));
    table("the fog LUT", render_pica.fog.lut.data(), r.fog.data(), sizeof(r.fog));
    const auto& t = render_pica.proctex;
    const u8* q = reinterpret_cast<const u8*>(r.proctex.data());
    for (const auto [data, bytes] :
         {std::pair<const void*, u32>{t.noise_table.data(), sizeof(t.noise_table)},
          {t.color_map_table.data(), sizeof(t.color_map_table)},
          {t.alpha_map_table.data(), sizeof(t.alpha_map_table)},
          {t.color_table.data(), sizeof(t.color_table)},
          {t.color_diff_table.data(), sizeof(t.color_diff_table)}}) {
        table("a proctex table", data, q, bytes);
        q += bytes;
    }
}

void RenderThread::AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                               const Pica::OutputVertex& v2) {
    // RasterizerGxm stops the emulator on it, with the draw's description.
    SyncState();
    Put(Op::AddTriangle, TriangleRecord{{v0, v1, v2}});
}

void RenderThread::DrawTriangles() {
    SyncState();
    Put(Op::DrawTriangles);
}

bool RenderThread::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
    SyncState();
    Put(Op::DisplayTransfer, config);
    return true;
}

bool RenderThread::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    SyncState();
    Put(Op::TextureCopy, config);
    return true;
}

bool RenderThread::AccelerateFill(const Pica::MemoryFillConfig& config) {
    SyncState();
    Put(Op::Fill, config);
    return true;
}

bool RenderThread::AccelerateDma(PAddr src, PAddr dst, u32 size) {
    SyncState();
    const u8* const bytes = check ? memory.GetPhysicalPointer(src) : nullptr;
    Put(Op::Dma, DmaRecord{src, dst, size, bytes ? Common::ComputeHash64(bytes, size) : 0});
    return true;
}

namespace {

/// A range of guest memory a draw's GPU work reads or writes.
struct GuestRange {
    PAddr addr;
    u32 bytes;
};

/// The guest bytes the enabled texture units sample: each unit's chain down to a side of 8, a
/// cube's for each face.
u32 TextureRanges(const Pica::RegsInternal& regs, std::array<GuestRange, 8>& out) {
    using TextureConfig = Pica::TexturingRegs::TextureConfig;
    using F = Pica::TexturingRegs::TextureFormat;
    u32 n = 0;
    const auto textures = regs.texturing.GetTextures();
    for (u32 unit = 0; unit < textures.size(); unit++) {
        const auto& t = textures[unit];
        if (!t.enabled) {
            continue;
        }
        const auto& c = t.config;
        const u32 bits = t.format == F::ETC1     ? 4
                         : t.format == F::ETC1A4 ? 8
                         : t.format > F::ETC1A4  ? 0
                                                 : Pica::TexturingRegs::NibblesPerPixel(t.format) * 4;
        const u32 width = c.width, height = c.height;
        u32 levels = 1;
        while (levels <= c.lod.max_level && width >> levels >= 8 && height >> levels >= 8) {
            levels++;
        }
        u32 bytes = 0;
        for (u32 l = 0; l < levels; l++) {
            bytes += (width >> l) * (height >> l) * bits / 8;
        }
        if (unit == 0 && c.type == TextureConfig::TextureCube) {
            using Face = Pica::TexturingRegs::CubeFace;
            static constexpr Face faces[6] = {Face::PositiveX, Face::NegativeX, Face::PositiveY,
                                              Face::NegativeY, Face::PositiveZ, Face::NegativeZ};
            for (const Face face : faces) {
                out[n++] = {regs.texturing.GetCubePhysicalAddress(face), bytes};
            }
        } else {
            out[n++] = {c.GetPhysicalAddress(), bytes};
        }
    }
    return n;
}

} // namespace

void RenderThread::KeepTargetUncached() {
    std::array<GuestRange, 8> textures;
    const u32 count = TextureRanges(pica.regs.internal, textures);
    const auto& fb = pica.regs.internal.framebuffer.framebuffer;
    const GuestRange colour{fb.GetColorBufferPhysicalAddress(),
                            fb.GetWidth() * fb.GetHeight() *
                                Pica::FramebufferRegs::BytesPerColorPixel(fb.color_format)};
    bool move = renderer.WouldMakeUncached(colour.addr, colour.bytes);
    for (u32 i = 0; i < count && !move; i++) {
        move = renderer.WouldMakeUncached(textures[i].addr, textures[i].bytes);
    }
    if (!move) {
        return;
    }
    Hold();
    for (u32 i = 0; i < count; i++) {
        renderer.MakeUncached(textures[i].addr, textures[i].bytes, false);
    }
    renderer.MakeUncached(colour.addr, colour.bytes, false);
    Release();
}

bool RenderThread::AccelerateDrawBatch(bool is_indexed) {
    KeepTargetUncached();
    SyncState();
    if (check) {
        PutCheck();
    }
    if (is_indexed && pica.regs.internal.pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes) {
        PutGsIndices();
    }
    // Whether it is indexed, in the head's extra field.
    Begin(Op::Draw, 0, is_indexed ? 1 : 0);
    Commit();
    // A geometry-shader draw's invocations leave b15 true (upstream GeometryPipeline::
    // SubmitVertex), here as on the render thread (Op::Draw); a late-dropped draw ran none.
    GsInvoked(pica);
    return true;
}

void RenderThread::PutGsIndices() {
    // The geometry stage reads a draw's indices on the CPU (each invocation's rows), as late as
    // the render thread runs behind: by then the guest may have written the next frame's, while
    // the vertex range went with the draw from these (a game' particles: rows outside
    // the vertices A0 stored, long triangles and lines for a frame while the GPU lagged).
    const auto& pipeline = pica.regs.internal.pipeline;
    const bool index_u16 = pipeline.index_array.format != 0;
    const u32 bytes = pipeline.num_vertices * (index_u16 ? 2u : 1u);
    const PAddr at =
        pipeline.vertex_attributes.GetPhysicalBaseAddress() + pipeline.index_array.offset;
    memory.GuardPhysicalRead(at, bytes);
    const u8* const indices = memory.GetPhysicalPointer(at);
    DEBUG_ASSERT_MSG(indices != nullptr, "render thread: geometry draw indices at {:#x}", at);
    std::memcpy(Begin(Op::GsIndices, bytes, index_u16 ? 1 : 0), indices, bytes);
    Commit();
}

bool RenderThread::AccelerateImmediateVertex(const Pica::AttributeBuffer& input) {
    SyncState();
    if (check) {
        PutCheck();
    }
    Put(Op::ImmediateVertex, input);
    return true;
}

void RenderThread::FlushImmediate() {
    KeepTargetUncached();
    SyncState();
    if (check) {
        PutCheck();
    }
    Put(Op::FlushImmediate);
}

void RenderThread::NotifyP3D() {
    SyncState();
    Put(Op::P3D);
}

void RenderThread::GpuIdle() {
    Put(Op::GpuIdle);
}

u64 RenderThread::InterruptFence() {
    const u32 ticket = ++tickets;
    Put(Op::Fence, ticket);
    return ticket;
}

bool RenderThread::InterruptPoll(u64 ticket) {
    return static_cast<s32>(reached.load(std::memory_order_acquire) - static_cast<u32>(ticket)) >=
           0;
}

void RenderThread::InterruptWait(u64 ticket) {
    if (InterruptPoll(ticket)) {
        return;
    }
    // Records the producer closed and has not published are not the render thread's yet, and a ticket
    // among them would never be reached.
    if (sceKernelGetThreadId() == producer && published.load(std::memory_order_relaxed) != write) {
        Publish();
    }
    const TraceSpan span{"emu_ticket", "emu_ticket_end"};
#ifdef VITA_DIAGNOSTICS
    const auto start = Clock::now();
#endif
    for (;;) {
        if (InterruptPoll(ticket)) {
#ifdef VITA_DIAGNOSTICS
            CountEmuWait(EmuTicket, start);
#endif
            return;
        }
        {
            // Held by this thread: nothing else polls; the fences are this thread's to wait for.
            if (Holding()) {
                while (!InterruptPoll(ticket)) {
                    PollFences();
                    if (InterruptPoll(ticket)) {
                        break;
                    }
                    ASSERT_MSG(!pending.empty(), "render thread: ticket {} never queued",
                               static_cast<u32>(ticket));
                    rasterizer.InterruptWait(pending.front().fence);
                }
#ifdef VITA_DIAGNOSTICS
                CountEmuWait(EmuTicket, start);
#endif
                return;
            }
        }
        // Another thread's hold (an HLE thread's wait for a held page while the emulation thread
        // holds): the holder's polls or, after its release, the render thread's reach the ticket.
        // This thread drives nothing: sgxd's waits take one waiter, and the fences are the
        // holder's (EVF_MULTI in a game).
        waited_ticket.store(static_cast<u32>(ticket), std::memory_order_relaxed);
        ticket_wait.store(true, std::memory_order_relaxed);
        std::scoped_lock lk{done};
        done_waiters.fetch_add(1);
        if (!InterruptPoll(ticket)) {
            SleepDone(EmuTicket);
        }
        done_waiters.fetch_sub(1);
        ticket_wait.store(false, std::memory_order_relaxed);
    }
}

std::vector<std::pair<PAddr, u32>> RenderThread::TakeCachedReads() {
    // RasterizerSgxd's list has its own lock.
    return rasterizer.TakeCachedReads();
}

void RenderThread::LoadDefaultDiskResources(const std::atomic_bool& stop_loading,
                                            const VideoCore::DiskResourceLoadCallback& callback) {
    RunSync([&] { rasterizer.LoadDefaultDiskResources(stop_loading, callback); });
}

void RenderThread::SwitchDiskResources(u64 title_id) {
    Run([this, title_id] { rasterizer.SwitchDiskResources(title_id); });
}

void RenderThread::SetRendererAccurateMul(bool accurate_mul) {
    Run([this, accurate_mul] { rasterizer.SetAccurateMul(accurate_mul); });
}

void RenderThread::Run(std::function<void()> fn) {
    CallRecord* const c = new CallRecord{std::move(fn), false};
    Put(Op::Call, c);
}

void RenderThread::RunSync(const std::function<void()>& fn) {
    if (thread.IsCurrentThread()) {
        fn();
        return;
    }
    const TraceSpan span{"emu_sync", "emu_sync_end"};
    CallRecord call{fn, true};
    {
        // Held, the record runs as it is written, here.
        CallRecord* const c = &call;
        Put(Op::Call, c);
    }
    // Inside a scan's PublishTogether the call is closed, not published: out with it, or it never runs.
    if (published.load(std::memory_order_relaxed) != write) {
        Publish();
    }
#ifdef VITA_DIAGNOSTICS
    const auto start = Clock::now();
#endif
    std::scoped_lock lk{done};
    done_waiters.fetch_add(1);
    while (!call.finished.load(std::memory_order_acquire)) {
        SleepDone(EmuSync);
    }
    done_waiters.fetch_sub(1);
#ifdef VITA_DIAGNOSTICS
    CountEmuWait(EmuSync, start);
#endif
}

void RenderThread::SleepDone([[maybe_unused]] EmuWait kind) {
#ifdef VITA_DIAGNOSTICS
    auto& c = emu_wait_counters[kind];
    c.sleeps.fetch_add(1, std::memory_order_relaxed);
    if (!done.WaitFor(std::chrono::milliseconds(1))) {
        c.timeouts.fetch_add(1, std::memory_order_relaxed);
    }
#else
    done.WaitFor(std::chrono::milliseconds(1));
#endif
}

#ifdef VITA_DIAGNOSTICS
void RenderThread::CountEmuWait(EmuWait kind, Clock::time_point start) {
    auto& c = emu_wait_counters[kind];
    c.waits.fetch_add(1, std::memory_order_relaxed);
    c.us.fetch_add(Since(start), std::memory_order_relaxed);
}
#endif

void RenderThread::Present(const std::array<Pica::FramebufferConfig, 2>& fb,
                           const std::array<Pica::ColorFill, 2>& fill) {
    const PresentRecord record{fb, fill};
    std::memcpy(Begin(Op::Present, sizeof(record)), &record, sizeof(record));
    // The record's position, before it is published.
    newest_present.store(write, std::memory_order_relaxed);
    Commit();
}

void RenderThread::Hold() {
    if (hold_depth++ != 0) {
        ASSERT_MSG(Holding(), "render thread: a hold taken on a second thread");
        return;
    }
    const TraceSpan span{"hold", "hold_parked"};
#ifdef VITA_DIAGNOSTICS
    const auto start = Clock::now();
#endif
    {
        std::scoped_lock lk{work};
        park_requested = true;
        work.NotifyAll();
    }
    {
        std::scoped_lock lk{done};
        done_waiters.fetch_add(1);
        while (!parked.load()) {
            done.WaitFor(std::chrono::milliseconds(1));
        }
        done_waiters.fetch_sub(1);
    }
    holder.store(sceKernelGetThreadId(), std::memory_order_release);
    // Waiters for tickets poll for themselves from now on.
    NotifyDone();
#ifdef VITA_DIAGNOSTICS
    holds.fetch_add(1, std::memory_order_relaxed);
    hold_us.fetch_add(Since(start), std::memory_order_relaxed);
#endif
}

void RenderThread::Release() {
    ASSERT(hold_depth != 0);
    if (--hold_depth != 0) {
        return;
    }
    SgxdTrace("release");
    holder.store(-1, std::memory_order_release);
    std::scoped_lock lk{work};
    park_requested = false;
    work.NotifyAll();
}

void RenderThread::PollHeld() {
    if (Holding()) {
        PollFences();
    }
}

bool RenderThread::Holding() const {
    return holder.load(std::memory_order_acquire) == sceKernelGetThreadId();
}

#ifdef VITA_DIAGNOSTICS
RenderThread::Stats RenderThread::TakeStats() {
    Stats s{};
    for (u32 k = 0; k < Stats::Count; k++) {
        s.records[k] = records[k].exchange(0);
    }
    s.busy_us = busy_us.exchange(0);
    s.idle_us = idle_us.exchange(0);
    s.poll_wakes = poll_wakes.exchange(0);
    s.presents = presents.exchange(0);
    s.presents_dropped = presents_dropped.exchange(0);
    s.holds = holds.exchange(0);
    s.hold_us = hold_us.exchange(0);
    for (u32 k = 0; k < EmuWaits; k++) {
        auto& c = emu_wait_counters[k];
        s.emu_waits[k] = c.waits.exchange(0);
        s.emu_wait_us[k] = c.us.exchange(0);
        s.emu_sleeps[k] = c.sleeps.exchange(0);
        s.emu_timeouts[k] = c.timeouts.exchange(0);
    }
    return s;
}
#endif

void RenderThread::NotifyDone() {
    if (done_waiters.load() != 0) {
        std::scoped_lock lk{done};
        done.NotifyAll();
    }
}

void RenderThread::RunAvailable() {
    // A Call's function may queue: the outer loop runs those.
    if (running_available) {
        return;
    }
    u32 r = read.load(std::memory_order_relaxed);
    if (r == published.load(std::memory_order_acquire)) {
        return;
    }
    running_available = true;
#ifdef VITA_DIAGNOSTICS
    const auto start = Clock::now();
#endif
    while (r != published.load(std::memory_order_acquire)) {
        Head head;
        const u8* const p = ring.get() + r % RingSize;
        std::memcpy(&head, p, sizeof(head));
        if (head.op != Op::Wrap) {
            RunRecord(head, p + sizeof(Head));
#ifdef VITA_DIAGNOSTICS
            records[KindOf(head.op)].fetch_add(1, std::memory_order_relaxed);
#endif
        }
        r += head.bytes;
        read.store(r, std::memory_order_release);
        if (space_waiting.load()) {
            NotifyDone();
        }
        if (!pending.empty()) {
            PollFences();
        }
    }
#ifdef VITA_DIAGNOSTICS
    busy_us.fetch_add(Since(start), std::memory_order_relaxed);
#endif
    running_available = false;
}

#ifdef VITA_DIAGNOSTICS
RenderThread::Stats::Kind RenderThread::KindOf(Op op) {
    switch (op) {
    case Op::Regs:
    case Op::Uniforms:
    case Op::Code:
    case Op::Swizzle:
    case Op::Lighting:
    case Op::Fog:
    case Op::ProcTex:
    case Op::DefaultAttributes:
        return Stats::State;
    case Op::Draw:
    case Op::MergedDraw:
    case Op::ImmediateVertex:
    case Op::FlushImmediate:
    case Op::AddTriangle:
    case Op::DrawTriangles:
        return Stats::Draw;
    case Op::Fill:
        return Stats::Fill;
    case Op::DisplayTransfer:
    case Op::TextureCopy:
        return Stats::Transfer;
    case Op::Dma:
        return Stats::Dma;
    case Op::Present:
        return Stats::Present;
    default:
        return Stats::Other;
    }
}
#endif

void RenderThread::RunRecord(const Head& head, const u8* p) {
    const auto take = [p]<typename T>(T& out) { std::memcpy(&out, p, sizeof(T)); };
    switch (head.op) {
    case Op::Wrap:
        break;
    case Op::Regs: {
        std::array<u64, 12> dirty;
        std::memcpy(dirty.data(), p, sizeof(dirty));
        const u8* values = p + sizeof(dirty);
        auto& regs = render_pica.regs.internal.reg_array;
        for (u32 w = 0; w < dirty.size(); w++) {
            for (u64 q = dirty[w]; q != 0; q &= q - 1) {
                std::memcpy(&regs[w * 64 + static_cast<u32>(std::countr_zero(q))], values,
                            sizeof(u32));
                values += sizeof(u32);
            }
            render_pica.dirty_regs.qwords[w] |= dirty[w];
        }
        break;
    }
    case Op::Uniforms: {
        Pica::ShaderSetup& setup = head.extra == 0 ? render_pica.vs_setup : render_pica.gs_setup;
        UniformsHead h;
        take(h);
        const u8* values = p + sizeof(h);
        for (u32 w = 0; w < 3; w++) {
            for (u32 bits = h.f[w]; bits != 0; bits &= bits - 1) {
                const u32 index = w * 32 + static_cast<u32>(std::countr_zero(bits));
                std::memcpy(&setup.uniforms.f[index], values, sizeof(setup.uniforms.f[0]));
                values += sizeof(setup.uniforms.f[0]);
#ifdef VITA_DIAGNOSTICS
                setup.f_stamp[index] = setup.f_version + 1;
#endif
            }
        }
        if (h.bool_int) {
            std::memcpy(setup.uniforms.b.data(), values, sizeof(setup.uniforms.b));
            std::memcpy(setup.uniforms.i.data(), values + sizeof(setup.uniforms.b),
                        sizeof(setup.uniforms.i));
        }
        setup.entry_point = h.entry_point;
        setup.uniforms_dirty = true;
        break;
    }
    case Op::Code:
    case Op::Swizzle: {
        Pica::ShaderSetup& setup = head.extra == 0 ? render_pica.vs_setup : render_pica.gs_setup;
        RangeHead h;
        take(h);
        const u32* const words = reinterpret_cast<const u32*>(p + sizeof(h));
        if (head.op == Op::Code) {
            setup.TakeProgramCode(h.lo, words, h.count, h.biggest);
        } else {
            setup.TakeSwizzleData(h.lo, words, h.count, h.biggest);
        }
        break;
    }
    case Op::Lighting: {
        u32 mask;
        take(mask);
        const u8* tables = p + sizeof(mask);
        for (u32 bits = mask; bits != 0; bits &= bits - 1) {
            const u32 t = static_cast<u32>(std::countr_zero(bits));
            std::memcpy(render_pica.lighting.luts[t].data(), tables, LightingTableBytes);
            tables += LightingTableBytes;
        }
        render_pica.lighting.lut_dirty |= mask;
        break;
    }
    case Op::Fog:
        std::memcpy(render_pica.fog.lut.data(), p, sizeof(render_pica.fog.lut));
        render_pica.fog.lut_dirty = true;
        break;
    case Op::ProcTex: {
        auto& t = render_pica.proctex;
        u32 mask;
        take(mask);
        const u8* q = p + sizeof(mask);
        for (const auto [data, bytes] :
             {std::pair<void*, u32>{t.noise_table.data(), sizeof(t.noise_table)},
              {t.color_map_table.data(), sizeof(t.color_map_table)},
              {t.alpha_map_table.data(), sizeof(t.alpha_map_table)},
              {t.color_table.data(), sizeof(t.color_table)},
              {t.color_diff_table.data(), sizeof(t.color_diff_table)}}) {
            std::memcpy(data, q, bytes);
            q += bytes;
        }
        t.table_dirty |= static_cast<u8>(mask);
        rasterizer.ProcTexTablesChanged();
        break;
    }
    case Op::DefaultAttributes:
        take(render_pica.input_default_attributes);
        break;
    case Op::GsIndices:
        rasterizer.GsIndices(p, head.extra != 0);
        break;
    case Op::Draw: {
        // RasterizerSgxd draws or stops the emulator; the emulation side was told it drew.
        [[maybe_unused]] const bool drawn = rasterizer.AccelerateDrawBatch(head.extra != 0);
        DEBUG_ASSERT(drawn);
        GsInvoked(render_pica);
        break;
    }
    case Op::MergedDraw: {
        u32 count;
        take(count);
        DrawMerged(reinterpret_cast<const u16*>(p + sizeof(count)), count);
        break;
    }
    case Op::ImmediateVertex: {
        Pica::AttributeBuffer input;
        take(input);
        [[maybe_unused]] const bool taken = rasterizer.AccelerateImmediateVertex(input);
        DEBUG_ASSERT(taken);
        break;
    }
    case Op::FlushImmediate:
        rasterizer.FlushImmediate();
        break;
    case Op::AddTriangle: {
        TriangleRecord t;
        take(t);
        rasterizer.AddTriangle(t.v[0], t.v[1], t.v[2]);
        break;
    }
    case Op::DrawTriangles:
        rasterizer.DrawTriangles();
        break;
    case Op::P3D:
        rasterizer.NotifyP3D();
        break;
    case Op::Fill: {
        Pica::MemoryFillConfig config;
        take(config);
        rasterizer.AccelerateFill(config);
        break;
    }
    case Op::DisplayTransfer:
    case Op::TextureCopy: {
        Pica::DisplayTransferConfig config;
        take(config);
        if (head.op == Op::DisplayTransfer) {
            rasterizer.AccelerateDisplayTransfer(config);
        } else {
            rasterizer.AccelerateTextureCopy(config);
        }
        break;
    }
    case Op::Dma: {
        DmaRecord d;
        take(d);
        if (d.src_hash != 0) {
            const u8* const bytes = memory.GetPhysicalPointer(d.src);
            static u32 changed = 0;
            if (Common::ComputeHash64(bytes, d.size) != d.src_hash && changed++ < 32) {
                LOG_ERROR(Render, "rendercheck: the DMA {:#010x} to {:#010x}, {} bytes: the guest "
                                  "rewrote its source before the copy ({} so far)",
                          d.src, d.dst, d.size, changed);
            }
        }
        rasterizer.AccelerateDma(d.src, d.dst, d.size);
        break;
    }
    case Op::GpuIdle:
        rasterizer.GpuIdle();
        break;
    case Op::Fence: {
        u32 ticket;
        take(ticket);
        pending.push_back({ticket, rasterizer.InterruptFence()});
        break;
    }
    case Op::Present: {
        PresentRecord r;
        take(r);
        renderer.PollWindow();
        last_window_poll = Clock::now();
        // A newer present already queued shows a newer picture.
        const u32 at = static_cast<u32>(p - sizeof(Head) - ring.get());
        if (at != newest_present.load(std::memory_order_relaxed) % RingSize) {
#ifdef VITA_DIAGNOSTICS
            presents_dropped.fetch_add(1, std::memory_order_relaxed);
#endif
            SgxdTrace("present_behind");
            break;
        }
        const auto now = Clock::now();
        if (now - last_present < std::chrono::microseconds(12000)) {
#ifdef VITA_DIAGNOSTICS
            presents_dropped.fetch_add(1, std::memory_order_relaxed);
#endif
            break;
        }
        last_present = now;
        GxmTrace("present");
        renderer.PresentFrame(r.fb, r.fill);
        GxmTrace("presented");
        rasterizer.GpuIdle();
#ifdef VITA_DIAGNOSTICS
        presents.fetch_add(1, std::memory_order_relaxed);
#endif
        break;
    }
    case Op::GuestWrote: {
        u32 addr;
        take(addr);
        rasterizer.GuestWrote(addr);
        break;
    }
    case Op::Check:
        RunCheck(p);
        break;
    case Op::Call: {
        CallRecord* c;
        take(c);
        c->fn();
        if (c->sync) {
            c->finished.store(true, std::memory_order_release);
            // The caller may be gone from here on.
            if (done_waiters.load() != 0) {
                std::scoped_lock lk{done};
                done.NotifyAll();
            }
        } else {
            delete c;
        }
        break;
    }
    }
}

void RenderThread::PollFences() {
    bool advanced = false;
    while (!pending.empty() && rasterizer.InterruptPoll(pending.front().fence)) {
        reached.store(pending.front().ticket, std::memory_order_release);
        SgxdTrace("reached", static_cast<unsigned>(pending.front().ticket));
        pending.pop_front();
        advanced = true;
    }
    if (advanced) {
        NotifyDone();
    }
}

void RenderThread::Loop() {
    Common::SetCurrentThreadRole(Common::ThreadRole::Render);
    u32 progress = 0;
    auto progress_at = Clock::now();
    for (;;) {
        RunAvailable();
        PollFences();
        // The emulation side waits for a ticket: its work's pending 3D finishes now.
        if (!pending.empty() && ticket_wait.load(std::memory_order_relaxed)) {
            const u32 waited = waited_ticket.load(std::memory_order_relaxed);
            for (const Pending& p : pending) {
                if (static_cast<s32>(p.ticket - waited) > 0) {
                    break;
                }
                rasterizer.InterruptKick(p.fence);
            }
            PollFences();
        }
        if (!pending.empty()) {
            const volatile SgxdEvents* const ev = sgxd_ptla_page();
            const u32 now_progress = ev ? ev->chains + ev->twod : 0;
            const auto now = Clock::now();
            if (now_progress != progress) {
                progress = now_progress;
                progress_at = now;
            } else if (now - progress_at > std::chrono::microseconds(sgxd_chain_stall_us)) {
                // Nothing completed for as long as sgxd's waits allow: the backend's own wait for the oldest
                // ticket, which stops the emulator with its post-mortem if the GPU has stopped.
                LOG_WARNING(Render,
                            "render thread: no GPU completion in {} ms with ticket {} waiting",
                            sgxd_chain_stall_us / 1000, pending.front().ticket);
                rasterizer.InterruptWait(pending.front().fence);
                PollFences();
                progress_at = Clock::now();
            }
        }
        // The window between presents: a poll may run the pause menu, so it goes outside the work lock.
        if (Clock::now() - last_window_poll >= WindowPollInterval) {
            renderer.PollWindow();
            last_window_poll = Clock::now();
        }
        std::scoped_lock lk{work};
        const auto empty = [this] {
            return read.load(std::memory_order_relaxed) == published.load();
        };
        if (park_requested && empty()) {
            // A hold: its thread is the render side until it lets go.
            parked.store(true);
            NotifyDone();
            while (park_requested) {
                work.WaitFor(std::chrono::seconds(1));
            }
            parked.store(false);
            continue;
        }
        if (stopping && empty()) {
            break;
        }
        sleeping.store(true);
        if (empty() && !park_requested && !stopping) {
            // With tickets pending, sleep only until the next poll: the GPU's completions come
            // to no one (sgxd's events page), so they are looked for.
            const bool polling = !pending.empty();
            const auto start = Clock::now();
            const bool woken = work.WaitFor(
                polling ? poll_interval
                        : std::chrono::duration_cast<std::chrono::microseconds>(WindowPollInterval));
            idle_us.fetch_add(Since(start), std::memory_order_relaxed);
            if (polling && !woken) {
                poll_wakes.fetch_add(1, std::memory_order_relaxed);
            }
        }
        sleeping.store(false);
    }
}

} // namespace SgxdRenderer
