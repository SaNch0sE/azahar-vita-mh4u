// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <tuple>
#include <type_traits>
#include <fmt/ranges.h>
#include "common/archives.h"
#include "common/assert.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/named_thread.h"
#include "core/memory.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"
#include "video_core/renderer_sgxd/render_thread.h"
#include "video_core/renderer_sgxd/renderer_sgxd.h"
#include "video_core/right_eye_disabler.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <psp2/kernel/error.h>
#include <psp2/kernel/threadmgr.h>
#include "sgxd.h"
#include "sgxd_mkq.h"
#include "sgxd_ptla.h"
#include "sgxd_session.h"

namespace SgxdRenderer {

enum class RenderThread::Op : u16 {
    /// The rest of the ring is unused: the next record is at its start.
    Wrap,
    // The emulation side's, for the parse thread (ParseRecord).
    ListDraws, ///< scan decisions of the next list's draws (RenderThread::ListDraw each)
    CmdList,   ///< CmdListRecord: the list to parse, its decisions in the ListDraws before it
    RegWrite,  ///< a register id and value the guest wrote outside a list
    // The parse thread's: the PICA state applied to the render side's PicaCore, and the rasterizer's
    // draw calls.
    Regs,     ///< the dirty table's 12 words, then each dirty register's value, in order
    Uniforms, ///< extra: the setup (0 VS, 1 GS); UniformsHead, then the values it names
    Code,     ///< extra: the setup; RangeHead, then the words
    Swizzle,  ///< extra: the setup; RangeHead, then the words
    Lighting, ///< the changed tables' mask, then each one's 256 words, in order
    Fog,      ///< the 128 words
    ProcTex,  ///< the dirty mask, then the five tables
    DefaultAttributes,
    Draw,       ///< extra: indexed
    GsIndices,  ///< extra: 16-bit; a geometry draw's indices as the scan read them
    MergedDraw, ///< MergedDrawHead, then its vertices' numbers (u16): RenderThread::MergeDraw
    ImmediateVertex,
    FlushImmediate,
    AddTriangle,
    DrawTriangles,
    LateDropped, ///< the late skip: a dropped draw's colour buffer and viewport
                 ///< (RasterizerSgxd::LateDropped)
    ListEnd,     ///< the list's parsed records end (RunParsed)
    P3D,
    Fill,
    DisplayTransfer,
    TextureCopy,
    Dma,
    GpuIdle,
    Fence,
    Present,
    FrameEnd,    ///< a guest frame's end, each vblank: RasterizerGxm::EndFrame
    GuestWrote,  ///< T12: a watched texture page's address, written by the guest or the HLE
    LateDropped, ///< The late skip: a dropped draw's colour buffer and viewport (RasterizerGxm::LateDropped)
    Call,
    PageRemap, ///< extra: the count; PageHolds::Remap each: the GPU's MMU onto pages' slots
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

struct CmdListRecord {
    PAddr addr;
    u32 size;
    u32 seq;    ///< the emulation side's count of the lists handed over
    u32 draws;  ///< its scan's decisions (ListDraws), for the parse's check
    u32 ranges; ///< the list's words and the buffers it jumps to, RenderThread::GuestRange each
                ///< after this
};

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

static_assert(std::is_trivially_copyable_v<Pica::OutputVertex>);
static_assert(std::is_trivially_copyable_v<Pica::AttributeBuffer>);
static_assert(std::is_trivially_copyable_v<Pica::Uniforms>);
static_assert(std::is_trivially_copyable_v<Pica::MemoryFillConfig>);
static_assert(std::is_trivially_copyable_v<Pica::DisplayTransferConfig>);
static_assert(std::is_trivially_copyable_v<PresentRecord>);

constexpr u32 LightingTableBytes = 256 * sizeof(u32);

// Not libstdc++'s steady_clock, which is a gettimeofday syscall on the Vita.
using Clock = Common::ProcessClock;

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

namespace {
std::atomic<RenderThread*> live_render_thread{};
std::atomic<u32> max_screen_skips{2};

/// sgxd_flags vbcached (GPU::VBlank's): geometry pages stay cached, a guest write to them too.
bool GeometryStaysCached() {
#ifdef VITA_DIAGNOSTICS
    static const bool cached = SgxdFlag("vbcached");
    return cached;
#else
    return false;
#endif
}
} // namespace

class RenderThread::ParsedOutput final : public VideoCore::RasterizerInterface {
public:
    explicit ParsedOutput(RenderThread& t_) : t{t_} {}
    void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                     const Pica::OutputVertex& v2) override {
        // RasterizerSgxd stops the emulator on it, with the draw's description.
        t.SyncParsedState();
        t.ParsedPut(Op::AddTriangle, TriangleRecord{{v0, v1, v2}});
    }
    void DrawTriangles() override {
        t.SyncParsedState();
        t.ParsedPut(Op::DrawTriangles);
    }
    void FlushAll() override {}
    void FlushRegion(PAddr, u32) override {}
    void InvalidateRegion(PAddr, u32) override {}
    void FlushAndInvalidateRegion(PAddr, u32) override {}
    void ClearAll(bool) override {}
    bool AccelerateDrawBatch(bool is_indexed) override {
        // A geometry draw whose state is the held run's joins it; another draw sends the run first.
        if (t.MergeDraw(is_indexed)) {
            return true;
        }
        t.FlushRun();
        const bool runs = t.RunsDraw(is_indexed);
        t.SyncParsedState();
        if (t.parse_gs_bytes != 0) {
            std::memcpy(t.ParsedBegin(Op::GsIndices, t.parse_gs_bytes, t.parse_gs_u16 ? 1 : 0),
                        t.parse_gs, t.parse_gs_bytes);
            t.ParsedCommit();
            t.parse_gs_bytes = 0;
        }
        if (runs) {
            // Held, with the state it was drawn in: its record goes with the run.
            const auto& pipeline = t.parse_pica.regs.internal.pipeline;
            t.held.draws = 1;
            t.held.first_indices = std::exchange(t.parse_indices, DrawIndexRange{});
            t.held.vertices.clear();
            for (u32 v = 0; v < pipeline.num_vertices; v++) {
                t.held.vertices.push_back(static_cast<u16>(pipeline.vertex_offset + v));
            }
            t.held.regs = t.parse_pica.regs.internal.reg_array;
            return true;
        }
        t.ParsedPut(Op::Draw, std::exchange(t.parse_indices, DrawIndexRange{}),
                    is_indexed ? 1 : 0);
        t.PublishParsed();
        return true;
    }
    bool AcceleratesGeometryShaders() const override {
        return true;
    }
    bool AccelerateImmediateVertex(const Pica::AttributeBuffer& input) override {
        t.SyncParsedState();
        t.ParsedPut(Op::ImmediateVertex, input);
        return true;
    }
    void FlushImmediate() override {
        t.SyncParsedState();
        t.ParsedPut(Op::FlushImmediate);
        t.PublishParsed();
    }
    void NotifyP3D() override {
        t.SyncParsedState();
        t.ParsedPut(Op::P3D);
    }

private:
    RenderThread& t;
};

RenderThread::RenderThread(Pica::PicaCore& pica_, Pica::PicaCore& parse_pica_,
                           Pica::PicaCore& render_pica_, RendererSgxd& renderer_,
                           RasterizerSgxd& rasterizer_, Memory::MemorySystem& memory_,
                           const VideoCore::RightEyeDisabler& right_eye_)
    : pica{pica_}, parse_pica{parse_pica_}, render_pica{render_pica_}, renderer{renderer_},
      rasterizer{rasterizer_}, memory{memory_}, right_eye_disabler{right_eye_},
      ring{std::make_unique<u8[]>(RingSize)}, page_holds{memory_},
      producer{sceKernelGetThreadId()} {
    static_assert(std::has_single_bit(RingSize), "positions wrap at 2^32");
    static_assert(std::has_single_bit(ParsedSize), "positions wrap at 2^32");
    static_assert(sizeof(Head) == 8, "records stay 8-byte aligned");
    parsed = std::make_unique<u8[]>(ParsedSize);
    parsed_output = std::make_unique<ParsedOutput>(*this);
    pica.SetSplit(Pica::CmdListMode::Scan, this);
    parse_pica.SetSplit(Pica::CmdListMode::Render, this);
    parse_pica.BindRasterizer(parsed_output.get());
    render_pica.BindRasterizer(&rasterizer);
    wake_flag = sceKernelCreateEventFlag("render wake", SCE_EVENT_WAITSINGLE, 0, nullptr);
    ASSERT_MSG(wake_flag >= 0, "render thread: event flag {:#010x}", static_cast<u32>(wake_flag));
    parse_wake_flag = sceKernelCreateEventFlag("parse wake", SCE_EVENT_WAITSINGLE, 0, nullptr);
    ASSERT_MSG(parse_wake_flag >= 0, "render thread: the parse thread's event flag {:#010x}",
               static_cast<u32>(parse_wake_flag));
    rasterizer.InterruptWaker(wake_flag, WakeBit);
    live_render_thread.store(this, std::memory_order_release);
    renderer.SetMoveHooks({
        [this](PAddr addr, u32 size) {
            // On the render thread, the move is for its work about to use the pages: the GPU
            // writes only moved pages and VRAM, so a page still cached has none queued but a
            // DMA's, which the draw retired already (RasterizerSgxd::MoveWaits).
            if (sceKernelGetThreadId() == render_tid.load(std::memory_order_relaxed)) {
                rasterizer.MoveWaits();
            } else {
                WaitGpuWrites(addr, size);
            }
        },
        [this](const u32* pte_va, const u32* pte, u32 count) {
            return rasterizer.RemapMoved(pte_va, pte, count);
        },
        [this](u32 fence) { return rasterizer.MovedFenceReached(fence); },
        [this](u32 fence) { rasterizer.MovedFenceWait(fence); },
        [this](u32 page) { return page_holds_on && page_holds.Versioned(page); },
    });
#ifdef VITA_DIAGNOSTICS
    gpu_wakes_mark = sgxd_wake_count();
#endif
    constexpr std::size_t StackSize = 2 * 1024 * 1024;
    thread = Common::NamedThread{Common::ThreadCfg{"render", StackSize}, [this] { Loop(); }};
    parse_thread =
        Common::NamedThread{Common::ThreadCfg{"cmdlist parse", 256 * 1024}, [this] { ParseLoop(); }};
    LOG_INFO(Render, "render thread: on Vita core 1, woken by sgxd when a ticket's GPU work is "
                     "done; the command lists parsed on core 0");
#ifdef VITA_DIAGNOSTICS
    if (SgxdFlag("perflog")) {
        gpu_sampler = Common::NamedThread{Common::ThreadCfg{"gpu sampler", 16 * 1024},
                                          [this](std::stop_token stop) { SampleGpu(stop); }};
    }
#endif
}

#ifdef VITA_DIAGNOSTICS
void RenderThread::SampleGpu(std::stop_token stop) {
    Common::SetCurrentThreadRole(Common::ThreadRole::Other);
    static_assert(GpuAtStates == SGXD_MKQ_WAITS + 1);
    static_assert(GpuLabels == SGXD_MKQ_LABELS);
    // A stream's state: 0 no job; 1 a job's code (or between jobs); 1 + its wait's kind.
    const auto state_of = [](u32 resume, u32 pos, u32 end) -> u32 {
        if (resume != 0) {
            const u32 kind = sgxd_mkq_wait_kind(resume);
            return kind == SGXD_MKQ_WAIT_NONE ? 1 : 1 + kind;
        }
        return pos != end ? 1 : 0;
    };
    u32 last_gpu_state = ~0u;
    while (!stop.stop_requested()) {
        sceKernelDelayThread(1000);
        // Busy: a submit or a PTLA op sent and not completed.
        const bool busy =
            sgxd_chain_done() != sgxd_chain_fence() || sgxd_ptla_done() != sgxd_ptla_fence();
        gpu_samples.fetch_add(1, std::memory_order_relaxed);
        if (busy) {
            gpu_busy.fetch_add(1, std::memory_order_relaxed);
        }
        const volatile u32* const mk = sgxd_mkq_state();
        const u32 ta = state_of(mk[SGXD_MK_RESUME], mk[SGXD_MK_POS], mk[SGXD_MK_END]);
        const u32 d3 = state_of(mk[SGXD_MK_RESUME3], mk[SGXD_MK_POS3], mk[SGXD_MK_END3]);
        gpu_at[d3 * GpuAtStates + ta].fetch_add(1, std::memory_order_relaxed);
        // The running job is the one before POS; a stream at its job's TA or 3D wait has that on the
        // hardware.
        if (d3 == 1 + SGXD_MKQ_WAIT_3D) {
            gpu_3d_by[sgxd_mkq_job_label(mk[SGXD_MK_POS3] - 1u)].fetch_add(
                1, std::memory_order_relaxed);
        }
        if (ta == 1 + SGXD_MKQ_WAIT_TA) {
            gpu_ta_by[sgxd_mkq_job_label(mk[SGXD_MK_POS] - 1u)].fetch_add(
                1, std::memory_order_relaxed);
        }
        // tracelog: the streams' states when they change (3D bits 15:8, TA 7:0, busy 16), each millisecond.
        const u32 state = (busy ? 1u << 16 : 0) | d3 << 8 | ta;
        if (state != last_gpu_state) {
            last_gpu_state = state;
            SgxdTrace("gpu", state);
        }
    }
}
#endif

RenderThread::~RenderThread() {
    live_render_thread.store(nullptr, std::memory_order_release);
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
    Wake();
    thread.join();
    parse_stopping.store(true);
    ParseWake();
    parse_thread.join();
    sceKernelDeleteEventFlag(parse_wake_flag);
    pica.SetSplit(Pica::CmdListMode::Whole, nullptr);
    parse_pica.SetSplit(Pica::CmdListMode::Whole, nullptr);
    // The backend may have given sgxd's interrupt back already, and the waker with it.
    rasterizer.InterruptWaker(-1, 0);
    sceKernelDeleteEventFlag(wake_flag);
}

namespace {
/// The dirty registers marked, counted with NEON (ARMv7 has no popcount).
u32 CountDirty(const Pica::DirtyRegs& dirty) {
#if defined(__ARM_NEON)
    static_assert(sizeof(dirty.qwords) % 16 == 0);
    const u8* const bytes = reinterpret_cast<const u8*>(dirty.qwords.data());
    uint16x8_t sums = vdupq_n_u16(0);
    for (std::size_t at = 0; at < sizeof(dirty.qwords); at += 16) {
        sums = vpadalq_u8(sums, vcntq_u8(vld1q_u8(bytes + at)));
    }
    const uint64x2_t pairs = vpaddlq_u32(vpaddlq_u16(sums));
    return static_cast<u32>(vget_lane_u64(vadd_u64(vget_low_u64(pairs), vget_high_u64(pairs)), 0));
#else
    u32 count = 0;
    for (const u64 q : dirty.qwords) {
        count += static_cast<u32>(std::popcount(q));
    }
    return count;
#endif
}
} // namespace

void RenderThread::SyncParsedState() {
    auto& dirty = parse_pica.dirty_regs;
    const u32 count = CountDirty(dirty);
    if (count != 0) {
        u8* p = ParsedBegin(Op::Regs, sizeof(dirty.qwords) + count * sizeof(u32));
        std::memcpy(p, dirty.qwords.data(), sizeof(dirty.qwords));
        u32* values = reinterpret_cast<u32*>(p + sizeof(dirty.qwords));
        // In 32-bit words: ARMv7 finds a word's lowest bit in two instructions.
        const u32* const regs = parse_pica.regs.internal.reg_array.data();
        u32 halves[sizeof(dirty.qwords) / sizeof(u32)];
        std::memcpy(halves, dirty.qwords.data(), sizeof(halves));
        for (u32 w = 0; w < std::size(halves); w++) {
            for (u32 h = halves[w]; h != 0; h &= h - 1) {
                *values++ = regs[w * 32 + static_cast<u32>(std::countr_zero(h))];
            }
        }
        ParsedCommit();
        dirty.Reset();
    }

    for (u16 unit = 0; unit < 2; unit++) {
        Pica::ShaderSetup& setup = unit == 0 ? parse_pica.vs_setup : parse_pica.gs_setup;
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
            u8* p = ParsedBegin(Op::Uniforms,
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
            ParsedCommit();
            setup.uniforms_dirty = false;
            setup.f_changed = {};
            setup.bi_changed = false;
        }
        if (setup.code_hi > setup.code_lo) {
            const RangeHead h{setup.code_lo, setup.code_hi - setup.code_lo,
                              setup.GetBiggestProgramSize()};
            u8* p = ParsedBegin(Op::Code, sizeof(h) + h.count * sizeof(u32), unit);
            std::memcpy(p, &h, sizeof(h));
            std::memcpy(p + sizeof(h), setup.GetProgramCode().data() + h.lo, h.count * sizeof(u32));
            ParsedCommit();
            setup.code_lo = Pica::MAX_PROGRAM_CODE_LENGTH;
            setup.code_hi = 0;
        }
        if (setup.swizzle_hi > setup.swizzle_lo) {
            const RangeHead h{setup.swizzle_lo, setup.swizzle_hi - setup.swizzle_lo,
                              setup.GetBiggestSwizzleSize()};
            u8* p = ParsedBegin(Op::Swizzle, sizeof(h) + h.count * sizeof(u32), unit);
            std::memcpy(p, &h, sizeof(h));
            std::memcpy(p + sizeof(h), setup.GetSwizzleData().data() + h.lo,
                        h.count * sizeof(u32));
            ParsedCommit();
            setup.swizzle_lo = Pica::MAX_SWIZZLE_DATA_LENGTH;
            setup.swizzle_hi = 0;
        }
    }

    if (const u32 mask = parse_pica.lighting.lut_dirty & Pica::PicaCore::Lighting::LutAllDirty) {
        const u32 tables = static_cast<u32>(std::popcount(mask));
        u8* p = ParsedBegin(Op::Lighting, sizeof(u32) + tables * LightingTableBytes);
        std::memcpy(p, &mask, sizeof(mask));
        p += sizeof(mask);
        for (u32 bits = mask; bits != 0; bits &= bits - 1) {
            const u32 t = static_cast<u32>(std::countr_zero(bits));
            std::memcpy(p, parse_pica.lighting.luts[t].data(), LightingTableBytes);
            p += LightingTableBytes;
        }
        ParsedCommit();
        parse_pica.lighting.lut_dirty = 0;
    }
    if (parse_pica.fog.lut_dirty) {
        u8* p = ParsedBegin(Op::Fog, sizeof(parse_pica.fog.lut));
        std::memcpy(p, parse_pica.fog.lut.data(), sizeof(parse_pica.fog.lut));
        ParsedCommit();
        parse_pica.fog.lut_dirty = false;
    }
    if (const u8 mask = parse_pica.proctex.table_dirty) {
        const auto& t = parse_pica.proctex;
        const u32 tables = sizeof(t.noise_table) + sizeof(t.color_map_table) +
                           sizeof(t.alpha_map_table) + sizeof(t.color_table) +
                           sizeof(t.color_diff_table);
        u8* p = ParsedBegin(Op::ProcTex, sizeof(u32) + tables);
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
        ParsedCommit();
        parse_pica.proctex.table_dirty = 0;
    }
    if (parse_pica.default_attributes_changed) {
        ParsedPut(Op::DefaultAttributes, parse_pica.input_default_attributes);
        parse_pica.default_attributes_changed = false;
    }
}

namespace {
u32 Older(u32 a, u32 b) {
    return static_cast<s32>(a - b) < 0 ? a : b;
}
} // namespace

u8* RenderThread::Begin(Op op, u32 bytes, u16 extra) {
    const u32 need = static_cast<u32>(sizeof(Head)) + Align8(bytes);
    ASSERT_MSG(need <= RingSize / 4, "render thread: a {}-byte record", need);
    u32 at = write % RingSize;
    // A record never wraps: the rest of the ring goes to a Wrap record. The room ends at the record the
    // render side or the parse thread reads next, whichever is older.
    const u32 skip = RingSize - at < need ? RingSize - at : 0;
    const auto oldest = [this] {
        return Older(read.load(std::memory_order_acquire),
                     parse_read.load(std::memory_order_acquire));
    };
    if (RingSize - (write - oldest()) < skip + need) {
        // The thread a whole ring behind: wait for it (a hold runs each record at once, so its
        // ring is always empty), with the records closed so far published.
        ASSERT(hold_depth == 0);
        const TraceSpan span{"emu_space", "emu_space_end"};
        if (published.load(std::memory_order_relaxed) != write) {
            Publish();
        }
        // The parse thread may sleep on records no list needed it for yet.
        ParseWake();
#ifdef VITA_DIAGNOSTICS
        const auto start = Clock::now();
#endif
        std::scoped_lock lk{done};
        done_waiters.fetch_add(1);
        space_waiting.store(true);
        while (RingSize - (write - oldest()) < skip + need) {
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
    // Sequentially consistent against `sleeping` and `parse_sleeping`: either the thread sees this
    // record before it sleeps, or this sees it sleeping and wakes it.
    published.store(write);
#ifdef VITA_DIAGNOSTICS
    publishes.fetch_add(1, std::memory_order_relaxed);
#endif
    if (parse_wake_due) {
        parse_wake_due = false;
        if (parse_sleeping.load()) {
            ParseWake();
        }
    }
    if (hold_depth != 0) {
        RunAvailable();
        return;
    }
    if (sleeping.load()) {
        Wake();
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

u8* RenderThread::ParsedBegin(Op op, u32 bytes, u16 extra) {
    if (held.draws != 0 && !held_flushing) {
        FlushRun();
    }
    const u32 need = static_cast<u32>(sizeof(Head)) + Align8(bytes);
    ASSERT_MSG(need <= ParsedSize / 4, "render thread: a {}-byte parsed record", need);
    u32 at = parsed_write % ParsedSize;
    const u32 skip = ParsedSize - at < need ? ParsedSize - at : 0;
    if (ParsedSize - (parsed_write - parsed_read.load(std::memory_order_acquire)) < skip + need) {
        // The render side a whole ring behind: the records so far go out, and this waits.
        if (parsed_published.load(std::memory_order_relaxed) != parsed_write) {
            parsed_published.store(parsed_write);
            if (sleeping.load()) {
                Wake();
            }
            if (done_waiters.load() != 0) {
                NotifyDone();
            }
        }
        parsed_space_waiting.store(true);
        while (ParsedSize - (parsed_write - parsed_read.load()) < skip + need) {
            ParseSleep(std::chrono::milliseconds(1));
        }
        parsed_space_waiting.store(false);
    }
    if (skip != 0) {
        const Head wrap{Op::Wrap, 0, skip};
        std::memcpy(parsed.get() + at, &wrap, sizeof(wrap));
        parsed_write += skip;
        at = 0;
    }
    const Head head{op, extra, need};
    std::memcpy(parsed.get() + at, &head, sizeof(head));
    parsed_open = need;
    return parsed.get() + at + sizeof(Head);
}

void RenderThread::ParsedCommit() {
    // Published a draw at a time and at the list's end: the render side runs records as they come.
    parsed_write += parsed_open;
}

template <typename T>
void RenderThread::ParsedPut(Op op, const T& payload, u16 extra) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(ParsedBegin(op, sizeof(T), extra), &payload, sizeof(T));
    ParsedCommit();
}

void RenderThread::ParsedPut(Op op) {
    ParsedBegin(op, 0);
    ParsedCommit();
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

// The draws are the lists' (SubmitCmdList): this thread's PicaCore makes them on the rasterizer.
void RenderThread::AddTriangle(const Pica::OutputVertex&, const Pica::OutputVertex&,
                               const Pica::OutputVertex&) {
    UNREACHABLE_MSG("render thread: a triangle outside a list's parse");
}

void RenderThread::DrawTriangles() {
    UNREACHABLE_MSG("render thread: a draw outside a list's parse");
}

namespace {
u32 TextureRanges(const Pica::RegsInternal& regs, RenderThread::DrawTextures& out);

/// After a draw: a geometry shader that ran leaves b15 true until the game writes it again.
void GsInvoked(Pica::PicaCore& p) {
    const auto& pipeline = p.regs.internal.pipeline;
    if (pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes && pipeline.num_vertices != 0) {
        p.gs_setup.uniforms.b[15] = true;
    }
}
} // namespace

void RenderThread::DrawMerged(const u16* vertices, u32 count) {
    // The run as one draw indexed by its vertices' numbers, 16-bit; while b15 is false, the run's first
    // draw alone first.
    auto& regs = render_pica.regs.internal.reg_array;
    constexpr u32 NumReg = PICA_REG_INDEX(pipeline.num_vertices);
    constexpr u32 IndexReg = PICA_REG_INDEX(pipeline.index_array);
    const u32 num = regs[NumReg], index = regs[IndexReg];
    for (u32 at = 0; at < count;) {
        const u32 n = at == 0 && !render_pica.gs_setup.uniforms.b[15] ? 1 : count - at;
        regs[NumReg] = n;
        regs[IndexReg] = index | 1u << 31;
        render_pica.dirty_regs.Set(NumReg);
        render_pica.dirty_regs.Set(IndexReg);
        rasterizer.GsIndices(reinterpret_cast<const u8*>(vertices + at), true);
        const auto [lo, hi] = std::minmax_element(vertices + at, vertices + at + n);
        rasterizer.ScannedIndices(*lo, *hi);
        [[maybe_unused]] const bool drawn = rasterizer.AccelerateDrawBatch(true);
        DEBUG_ASSERT(drawn);
        GsInvoked(render_pica);
        at += n;
    }
    regs[NumReg] = num;
    regs[IndexReg] = index;
    render_pica.dirty_regs.Set(NumReg);
    render_pica.dirty_regs.Set(IndexReg);
#ifdef VITA_DIAGNOSTICS
    merged_draws.fetch_add(count, std::memory_order_relaxed);
    merged_runs.fetch_add(1, std::memory_order_relaxed);
#endif
}

namespace {

/// The bytes a texture copy's lines span on one side: whole lines of `width` bytes `gap` apart (16-byte
/// units), or the size itself with no width.
u32 CopySpan(u32 size, u32 width, u32 gap) {
    if (width == 0) {
        return size;
    }
    const u32 w = width * 16;
    return (size + w - 1) / w * (w + gap * 16);
}

} // namespace

bool RenderThread::ReadsDroppedOutput(PAddr lo, PAddr hi) const {
    return std::any_of(dropped_outputs.begin(), dropped_outputs.end(),
                       [&](const DroppedOutput& o) { return lo < o.hi && o.lo < hi; });
}

void RenderThread::ForgetDroppedOutputs(PAddr lo, PAddr hi) {
    // What a kept write leaves of each output it reaches: the part before it, the part after.
    const std::size_t count = dropped_outputs.size();
    for (std::size_t i = 0; i < count; i++) {
        DroppedOutput& o = dropped_outputs[i];
        if (!(lo < o.hi && o.lo < hi)) {
            continue;
        }
        const DroppedOutput after{hi, o.hi, o.earlier};
        o.hi = std::max(o.lo, std::min(o.hi, lo));
        if (after.lo < after.hi) {
            dropped_outputs.push_back(after);
        }
    }
    std::erase_if(dropped_outputs, [](const DroppedOutput& o) { return o.lo >= o.hi; });
}

bool RenderThread::LateTransfer(const Pica::DisplayTransferConfig& config, bool display) {
    const PAddr in = config.GetPhysicalInputAddress();
    PAddr lcd = 0;
    const int screen = LcdScreenOf(config.GetPhysicalOutputAddress(),
                                   display ? config.OutputBytes() : config.texture_copy.size, &lcd);
    // A transfer from a colour buffer drawn lately ends its segment: the segment gets the transfer's
    // screen, and the transfer is dropped when a draw of the segment was (the buffer does not hold
    // the picture). A display transfer into a right-eye LCD buffer is left out, and its segment becomes
    // the right eye's, whose draws LateDraw leaves out from the next frame on.
    const bool right_eye =
        display && RightEyeOutput(config.GetPhysicalOutputAddress(), config.OutputBytes());
    // A top screen's picture into a buffer no screen shows: a right eye the LCD never shows, when a
    // transfer of the same buffer's other bytes goes to a screen (both eyes in one buffer).
    const bool hidden_right = display && !right_eye && screen < 0 &&
                              right_eye_disabler.Allowed() && config.output_width == 240 &&
                              config.output_height == 400;
    bool dropped = false;
    // The source: the colour buffer the transfer reads as the draws found it (by its address and the
    // size it reads, then its whole input's), else the newest drawn of the buffers holding its address
    // (a buffer drawn over another's memory, or two screens in one buffer).
    const u32 in_bytes = display ? config.input_width * config.input_height *
                                       Pica::BytesPerPixel(config.input_format)
                                 : config.texture_copy.size;
    const u32 read_bytes =
        display ? config.output_height * (config.scaling == config.ScaleXY ? 2u : 1u) *
                      config.input_width * Pica::BytesPerPixel(config.input_format)
                : in_bytes;
    SegmentTarget* source = nullptr;
    for (const u32 bytes : {read_bytes, in_bytes}) {
        for (SegmentTarget& t : segment_targets) {
            if (t.addr == in && t.bytes == bytes) {
                source = &t;
                break;
            }
        }
        if (source != nullptr) {
            break;
        }
    }
    if (source == nullptr) {
        for (SegmentTarget& t : segment_targets) {
            if (in >= t.addr && in < t.addr + t.bytes &&
                (source == nullptr || t.drawn > source->drawn)) {
                source = &t;
            }
        }
    }
    if (source) {
        source->screens[std::min<u32>(source->segment, SegmentSlots - 1)] =
            right_eye ? RightEyeScreen : static_cast<s8>(screen);
        source->to_right_eye |= right_eye;
        source->to_screen |= !right_eye && screen >= 0;
        // The bytes the transfer reads from the buffer: output_height rows from the input, input_width
        // apart; a texture copy to a screen the whole buffer.
        const auto reach = [](u32& lo, u32& hi, u32 from, u32 to) {
            lo = lo < hi ? std::min(lo, from) : from;
            hi = std::max(hi, to);
        };
        const u32 row = config.input_width * Pica::BytesPerPixel(config.input_format);
        const u32 from = in - source->addr;
        const u32 to = std::min(source->bytes, from + config.output_height * row);
        if (right_eye) {
            reach(source->to_right_lo, source->to_right_hi, from, to);
        } else if (screen >= 0) {
            reach(source->to_screen_lo, source->to_screen_hi, display ? from : 0,
                  display ? to : source->bytes);
        } else if (segment_copies.size() < 32) {
            if (hidden_right) {
                reach(source->to_hidden_lo, source->to_hidden_hi, from, to);
            }
            const PAddr out = config.GetPhysicalOutputAddress();
            const auto& tc = config.texture_copy;
            const u32 out_bytes = display ? config.OutputBytes()
                                          : CopySpan(tc.size, tc.output_width, tc.output_gap);
            segment_copies.push_back({out, out + out_bytes, source->addr, source->bytes});
        }
        dropped = source->dropped;
        source->segment++;
        source->dropped = false;
        source->open = false;
    } else {
        // From no drawn colour buffer: from a dropped transfer's output, a picture never made.
        const auto& tc = config.texture_copy;
        dropped = ReadsDroppedOutput(
            in, in + (display ? in_bytes : CopySpan(tc.size, tc.input_width, tc.input_gap)));
    }
    const bool lcd_dropped =
        screen >= 0 && std::find(dropped_lcd.begin(), dropped_lcd.end(), lcd) != dropped_lcd.end();
    dropped |= lcd_dropped;
    {
        const PAddr out = config.GetPhysicalOutputAddress();
        const auto& tc = config.texture_copy;
        const PAddr out_end =
            out + (display ? config.OutputBytes() : CopySpan(tc.size, tc.output_width, tc.output_gap));
        ForgetDroppedOutputs(out, out_end);
        if (dropped) {
            // Not an LCD buffer: it keeps its last whole picture, which a copy of it may read.
            if (screen < 0) {
                dropped_outputs.push_back({out, out_end});
            }
            // Into a colour buffer drawn lately: as a dropped draw of its segment, whose transfer goes too.
            for (SegmentTarget& t : segment_targets) {
                if (out < t.addr + t.bytes && t.addr < out_end) {
                    t.dropped = true;
                }
            }
        }
    }
#ifdef VITA_DIAGNOSTICS
    if (screenlog_on) {
        screenlog_transfers +=
            fmt::format(" {}{:#x}+{:#x}->{:#x}(s{}{}{})", display ? "" : "copy ", in, in_bytes,
                        config.GetPhysicalOutputAddress(), screen, dropped ? " dropped" : "",
                        right_eye ? " right" : "");
    }
#endif
    if (screen >= 0) {
        NoteLcdWrite(screen, lcd, dropped);
        if (dropped && !lcd_dropped && dropped_lcd.size() < 8) {
            dropped_lcd.push_back(lcd);
        }
    }
#ifdef VITA_DIAGNOSTICS
    static u32 logged = 0;
    if ((dropped || late_skip) && LatelogNow() && logged++ < 400) {
        LOG_INFO(Render,
                 "latelog: frame {}: {} {:#x}->{:#x} ({} bytes out), source {:#x}+{:#x} "
                 "screen {}, LCD screen {}{}",
                 screenlog_frame, display ? "display transfer" : "texture copy", in,
                 config.GetPhysicalOutputAddress(),
                 display ? config.OutputBytes() : static_cast<u32>(config.texture_copy.size),
                 source ? source->addr : 0, source ? source->bytes : 0,
                 source ? source->screens[std::min<u32>(source->segment - 1, SegmentSlots - 1)]
                        : -1,
                 screen, dropped ? ", dropped" : "");
    }
#endif
    if (right_eye) {
#ifdef VITA_DIAGNOSTICS
        right_eye_transfers.fetch_add(1, std::memory_order_relaxed);
#endif
        return true;
    }
    return dropped;
}

bool RenderThread::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
    if (LateTransfer(config, true)) {
        return true;
    }
    // The output's pages move to CDRAM without their contents: only the bytes the transfer writes
    // (OutputBytes, the scaling's). After the late skip's answer: a dropped transfer's pages would
    // hold the pool's old bytes.
    if (const u32 out_bytes = config.OutputBytes();
        renderer.WouldMakeUncached(config.GetPhysicalOutputAddress(), out_bytes)) {
        RunSync([&] {
            renderer.MakeUncached(config.GetPhysicalOutputAddress(), out_bytes, true);
        });
    }
    HoldRead(config.GetPhysicalInputAddress(),
             config.input_width * config.input_height * Pica::BytesPerPixel(config.input_format),
             PageHolds::TransferIn);
    // An LCD buffer a present named is the display's: its page opens past its bytes. Another output is
    // a transfer's like any (a game may read a converted picture back).
    const PAddr output = config.GetPhysicalOutputAddress();
    HoldWrite(output, config.OutputBytes(),
              LcdScreenOf(output, config.OutputBytes()) >= 0 ? PageHolds::DisplayOut
                                                             : PageHolds::TransferOut);
    Put(Op::DisplayTransfer, config);
    return true;
}

bool RenderThread::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    if (LateTransfer(config, false)) {
        return true;
    }
    const auto& tc = config.texture_copy;
    HoldRead(config.GetPhysicalInputAddress(), CopySpan(tc.size, tc.input_width, tc.input_gap),
             PageHolds::TransferIn);
    HoldWrite(config.GetPhysicalOutputAddress(), CopySpan(tc.size, tc.output_width, tc.output_gap),
              PageHolds::TransferOut);
    Put(Op::TextureCopy, config);
    return true;
}

bool RenderThread::AccelerateFill(const Pica::MemoryFillConfig& config) {
#ifdef VITA_DIAGNOSTICS
    static u32 logged = 0;
    if (late_skip && LatelogNow() && logged++ < 400) {
        LOG_INFO(Render, "latelog: frame {}: fill {:#x}-{:#x} in a late frame", screenlog_frame,
                 config.GetStartAddress(), config.GetEndAddress());
    }
    if (screenlog_on) {
        screenlog_transfers +=
            fmt::format(" fill {:#x}-{:#x}", config.GetStartAddress(), config.GetEndAddress());
    }
#endif
    ForgetDroppedOutputs(config.GetStartAddress(), config.GetEndAddress());
    HoldWrite(config.GetStartAddress(), config.GetEndAddress() - config.GetStartAddress(),
              PageHolds::FillOut);
    Put(Op::Fill, config);
    return true;
}

bool RenderThread::AccelerateDma(PAddr src, PAddr dst, u32 size) {
    HoldRead(src, size, PageHolds::DmaIn);
    HoldWrite(dst, size, PageHolds::DmaOut);
    ForgetDroppedOutputs(dst, dst + size);
    // A DMA into an LCD buffer writes that screen, as a transfer does.
    PAddr lcd = 0;
    const int screen = LcdScreenOf(dst, size, &lcd);
    if (screen >= 0) {
        NoteLcdWrite(screen, lcd, false);
    }
#ifdef VITA_DIAGNOSTICS
    if (screenlog_on) {
        screenlog_transfers += fmt::format(" dma {:#x}+{:#x}->{:#x}(s{})", src, size, dst, screen);
    }
#endif
    Put(Op::Dma, DmaRecord{src, dst, size});
    return true;
}

namespace {

/// The guest bytes the enabled texture units sample: each unit's chain down to a side of 8, a
/// cube's for each face.
using GuestRange = RenderThread::GuestRange;

u32 TextureRanges(const Pica::RegsInternal& regs, RenderThread::DrawTextures& out) {
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

void RenderThread::HoldRead(PAddr addr, u32 bytes, PageHolds::Kind kind) {
    if (page_holds_on) {
        page_holds.Read(addr, bytes, HoldTicket(), kind);
        PutPageRemaps();
    }
}

void RenderThread::HoldWrite(PAddr addr, u32 bytes, PageHolds::Kind kind) {
    last_write_ticket = HoldTicket();
    if (page_holds_on) {
        page_holds.Write(addr, bytes, HoldTicket(), kind);
        PutPageRemaps();
    }
}

void RenderThread::PutPageRemaps() {
    // Before the record of the work that uses the pages, as many to a list as it takes.
    auto& remaps = page_holds.Remaps();
    for (std::size_t at = 0; at < remaps.size(); at += SGXD_REMAP_MAX) {
        const u32 n = static_cast<u32>(std::min<std::size_t>(SGXD_REMAP_MAX, remaps.size() - at));
        u8* const out = Begin(Op::PageRemap, n * sizeof(PageHolds::Remap), static_cast<u16>(n));
        std::memcpy(out, remaps.data() + at, n * sizeof(PageHolds::Remap));
        Commit();
    }
    remaps.clear();
}

void RenderThread::StartFramePageHolds() {
    held_colour = {};
    held_depth = {};
    if (page_holds_on) {
        page_holds.StartFrame(reached.load(std::memory_order_acquire), HoldTicket());
        PutPageRemaps();
    }
}

void RenderThread::HoldDraw(const DrawTextures& textures, u32 count, bool vertices,
                            bool is_indexed) {
    if (!page_holds_on) {
        return;
    }
    const auto& regs = pica.regs.internal;
    for (u32 i = 0; i < count; i++) {
        HoldRead(textures[i].addr, textures[i].bytes, PageHolds::Texture);
        // Watched until the guest writes them, which marks the textures over them.
        page_holds.Watch(textures[i].addr, textures[i].bytes);
    }
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& om = regs.framebuffer.output_merger;
    const u32 pixels = fb.GetWidth() * fb.GetHeight();
    const auto once = [&](BufferHold& held, PAddr addr, u32 bytes) {
        if (held.addr == addr && held.bytes == bytes && held.ticket == HoldTicket()) {
            return false;
        }
        held = {addr, bytes, HoldTicket()};
        return true;
    };
    const PAddr colour = fb.GetColorBufferPhysicalAddress();
    const u32 colour_bytes = pixels * Pica::FramebufferRegs::BytesPerColorPixel(fb.color_format);
    if (once(held_colour, colour, colour_bytes)) {
        HoldWrite(colour, colour_bytes, PageHolds::Colour);
    }
    // Depth is loaded from the guest's buffer when a pass starts from it there, and stored in
    // the rasterizer's own depth stores, never into guest memory.
    if (om.depth_test_enable || (fb.allow_depth_stencil_write && om.depth_write_enable) ||
        om.stencil_test.enable) {
        const PAddr depth = fb.GetDepthBufferPhysicalAddress();
        const u32 depth_bytes = pixels * Pica::FramebufferRegs::BytesPerDepthPixel(fb.depth_format);
        if (once(held_depth, depth, depth_bytes)) {
            HoldRead(depth, depth_bytes, PageHolds::Depth);
        }
    }
    if (vertices) {
        HoldVertices(is_indexed);
    }
}

void RenderThread::HoldVertices(bool is_indexed) {
    const auto& pipeline = pica.regs.internal.pipeline;
    const auto& va = pipeline.vertex_attributes;
    const u32 count = pipeline.num_vertices;
    if (count == 0) {
        return;
    }
    const PAddr base = va.GetPhysicalBaseAddress();
    u32 lo = pipeline.vertex_offset;
    u32 hi = lo + count - 1;
    draw_indices = {};
    if (is_indexed) {
        const PAddr index_address = base + pipeline.index_array.offset;
        const bool index_u16 = pipeline.index_array.format != 0;
        HoldRead(index_address, count * (index_u16 ? 2 : 1), PageHolds::Index);
        // Every vertex an index can reach, but where a 16-bit index reaches into FCRAM: there the indices
        // are scanned. VRAM is held for reads whole.
        lo = 0;
        hi = index_u16 ? 0xffffu : 0xffu;
        bool fcram = false;
        for (const auto& loader : va.attribute_loaders) {
            const PAddr at = base + loader.data_offset;
            fcram |= loader.component_count != 0 && at >= Memory::FCRAM_PADDR &&
                     at < Memory::FCRAM_PADDR_END;
        }
        if (index_u16 && fcram) {
            u32 first, pages;
            const bool in_fcram = page_holds_on &&
                                  page_holds.PageRange(index_address, count * 2, first, pages);
            const bool memoable = in_fcram && page_holds.IndexMemoable(first, pages);
            IndexMemo& memo = index_memo[((index_address >> 1) ^ (count * 0x9E3779B1u)) %
                                         index_memo.size()];
            u16 a, b;
            if (memoable && memo.addr == index_address && memo.count == count &&
                memo.stamp == page_holds.IndexStamp(first, pages)) {
                a = memo.lo;
                b = memo.hi;
#ifdef VITA_DIAGNOSTICS
                index_memo_hits.fetch_add(1, std::memory_order_relaxed);
#endif
            } else {
                memory.GuardPhysicalRead(index_address, count * 2);
                const u8* const indices = memory.GetPhysicalPointer(index_address);
                DEBUG_ASSERT_MSG(indices != nullptr && (index_address & 1) == 0,
                                 "render thread: 16-bit indices at {:#x}", index_address);
#ifdef VITA_DIAGNOSTICS
                const auto scan_start = Clock::now();
#endif
                std::tie(a, b) = Common::FindMinMax(
                    std::span<const u16>{reinterpret_cast<const u16*>(indices), count});
#ifdef VITA_DIAGNOSTICS
                index_scan_us.fetch_add(Since(scan_start), std::memory_order_relaxed);
                index_scans.fetch_add(1, std::memory_order_relaxed);
                index_scan_indices.fetch_add(count, std::memory_order_relaxed);
#endif
                if (memoable) {
                    page_holds.WatchIndices(first, pages);
                    memo = {index_address, count, page_holds.IndexStamp(first, pages), a, b};
                }
            }
            lo = a;
            hi = b;
            draw_indices = {a, b, 1};
        }
    }
    for (const auto& loader : va.attribute_loaders) {
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }
        const u32 stride = loader.byte_count;
        HoldRead(base + loader.data_offset + lo * stride, (hi - lo + 1) * stride,
                 PageHolds::Vertex);
    }
}

bool RenderThread::AccelerateDrawBatch(bool) {
    UNREACHABLE_MSG("render thread: a draw outside a list's parse");
}

bool RenderThread::AccelerateImmediateVertex(const Pica::AttributeBuffer&) {
    UNREACHABLE_MSG("render thread: an immediate vertex outside a list's parse");
}

void RenderThread::FlushImmediate() {
    UNREACHABLE_MSG("render thread: an immediate batch outside a list's parse");
}

void RenderThread::NotifyP3D() {
    Put(Op::P3D);
}

void RenderThread::SubmitCmdList(PAddr addr, u32 size, bool ignore_list, std::vector<u64>& irqs) {
    // transfer, which looked like the right eye's: left out, its bottom screen was drawn into
    // the top screen's height, offset in the buffer. Every right-eye list scanned with its draws
    // dropped cost a game's race 195% -> 168%.
    DEBUG_ASSERT(!ignore_list || pica.CmdListMayDraw(addr, size));
    if (ignore_list) {
        pica.ProcessCmdList(addr, size, true);
        return;
    }
    // The scan's records (its holds' remaps, the decisions) and the list go out together.
    const PublishTogether together{*this};
    const TraceSpan span{"scan", "scan_end"};
    scan_draws.clear();
    scan_draw_count = 0;
    scan_lists.clear();
    scan_offsets.clear();
    scan_words_size = 0;
    scan_irqs = &irqs;
#ifdef VITA_DIAGNOSTICS
    const auto scan_start = Clock::now();
#endif
    pica.ProcessCmdList(addr, size, false);
    scan_irqs = nullptr;
    FlushScanDraws();
#ifdef VITA_DIAGNOSTICS
    {
        scan_us.fetch_add(Since(scan_start), std::memory_order_relaxed);
        u32 parsed = 0;
        for (const GuestRange& r : scan_lists) {
            parsed += r.bytes;
        }
        lists.fetch_add(1, std::memory_order_relaxed);
        list_bytes.fetch_add(size, std::memory_order_relaxed);
        list_parsed_bytes.fetch_add(parsed, std::memory_order_relaxed);
        list_ranges.fetch_add(static_cast<u32>(scan_lists.size()), std::memory_order_relaxed);
        list_draws.fetch_add(scan_draw_count, std::memory_order_relaxed);
        if (parsed > list_parsed_max.load(std::memory_order_relaxed)) {
            list_parsed_max.store(parsed, std::memory_order_relaxed);
        }
    }
#endif
    const u32 seq = ++list_seq;
    // The words the parse reads in place: until it has copied them, a guest write to their pages takes
    // a copy for it (ParseFault).
    if (page_holds_on) {
        std::vector<GuestRange> held;
        held.reserve(scan_lists.size());
        for (std::size_t i = 0; i < scan_lists.size(); i++) {
            if (scan_offsets[i] == NotCopied) {
                page_holds.HoldParse(scan_lists[i].addr, scan_lists[i].bytes);
                held.push_back(scan_lists[i]);
            }
        }
        parse_holds.push_back({seq, std::move(held), {}});
    }
    const CmdListRecord record{addr, size, seq, scan_draw_count,
                               static_cast<u32>(scan_lists.size())};
    const u32 ranges_bytes = static_cast<u32>(scan_lists.size() * sizeof(GuestRange));
    const u32 offsets_bytes = Align8(static_cast<u32>(scan_offsets.size() * sizeof(u32)));
    u8* const out = Begin(Op::CmdList, static_cast<u32>(sizeof(record)) + ranges_bytes +
                                           offsets_bytes + scan_words_size);
    std::memcpy(out, &record, sizeof(record));
    std::memcpy(out + sizeof(record), scan_lists.data(), ranges_bytes);
    std::memcpy(out + sizeof(record) + ranges_bytes, scan_offsets.data(),
                scan_offsets.size() * sizeof(u32));
    std::memcpy(out + sizeof(record) + ranges_bytes + offsets_bytes, scan_words.get(),
                scan_words_size);
    parse_wake_due = true;
    Commit();
}

void RenderThread::RegWrite(u32 id, u32 value) {
    const std::array<u32, 2> w{id, value};
    std::memcpy(Begin(Op::RegWrite, sizeof(w)), &w, sizeof(w));
    // Closed, not published: the next record's publish takes it along.
    write += open_record;
}

namespace {

/// A PicaCore's list state from one to the other through its savestate form; `to` keeps its LCD and
/// external registers and its delay generator.
void CopyListState(Pica::PicaCore& from, Pica::PicaCore& to) {
    const Pica::RegsLcd lcd = to.regs_lcd;
    constexpr std::size_t External = GPU_REG_INDEX(internal);
    std::array<u32, External> external;
    std::copy_n(to.regs.reg_array.begin(), External, external.begin());
    const Pica::DelayGenerator delay = to.delay_generator;
    std::stringstream stream;
    {
        oarchive out{stream};
        out & from;
    }
    {
        iarchive in{stream};
        in & to;
    }
    to.regs_lcd = lcd;
    std::copy_n(external.begin(), External, to.regs.reg_array.begin());
    to.delay_generator = delay;
}

} // namespace

void RenderThread::TakeWholeState() {
    // A loaded state's memory: no index range of the memo holds.
    index_memo.fill({});
    // Once the parse thread has read everything queued: it takes the lists after this with
    // the new state, all of it marked changed for the render side's PicaCore (its next
    // records).
    RunSync([this] {
        WaitParseIdle();
        CopyListState(pica, parse_pica);
    });
}

void RenderThread::GiveWholeState() {
    RunSync([this] {
        WaitParseIdle();
        CopyListState(parse_pica, pica);
    });
}

const u8* RenderThread::ScanList(PAddr addr, u32 size) {
    // A list, or a buffer it jumps to, a queued DMA still writes waits for it (a game Triple
    // Deluxe's lists jump into VRAM its DMAs fill each frame: a stale buffer there ended the
    // list before its P3D, and the game waited for that P3D for good).
    memory.GuardPhysicalRead(addr, size);
    scan_lists.push_back({addr, size});
    const u8* const guest = memory.GetPhysicalPointer(addr);
    if (addr < Memory::VRAM_PADDR || addr >= Memory::VRAM_PADDR_END) {
        scan_offsets.push_back(NotCopied);
        return guest;
    }
    const u32 at = scan_words_size;
    const u32 end = at + Align8(size);
    if (end > scan_words_cap) {
        const u32 cap = std::max(end, scan_words_cap * 2);
        std::unique_ptr<u8[]> grown(new u8[cap]);
        std::memcpy(grown.get(), scan_words.get(), at);
        scan_words = std::move(grown);
        scan_words_cap = cap;
    }
    std::memcpy(scan_words.get() + at, guest, size);
#ifdef VITA_DIAGNOSTICS
    if (static const bool listdiff = SgxdFlag("listdiff"); listdiff) {
        ListDiff(addr, scan_words.get() + at, size);
    }
#endif
    scan_words_size = end;
    scan_offsets.push_back(at);
    return scan_words.get() + at;
}

void RenderThread::ScanDraw(bool is_indexed) {
    ListDraw d{};
    if (LateDraw(d, pica.regs.internal.pipeline.num_vertices)) {
        PutScanDraw(d, nullptr);
        return;
    }
    // The textures' pages are held here; the render thread moves them to CDRAM at the untile.
    DrawTextures textures;
    const u32 count = TextureRanges(pica.regs.internal, textures);
    HoldDraw(textures, count, true, is_indexed);
    d.indices = std::exchange(draw_indices, DrawIndexRange{});
    const auto& pipeline = pica.regs.internal.pipeline;
    if (!is_indexed || pipeline.use_gs != Pica::PipelineRegs::UseGS::Yes) {
        PutScanDraw(d, nullptr);
        return;
    }
    const bool index_u16 = pipeline.index_array.format != 0;
    const PAddr at =
        pipeline.vertex_attributes.GetPhysicalBaseAddress() + pipeline.index_array.offset;
    d.gs_bytes = pipeline.num_vertices * (index_u16 ? 2u : 1u);
    d.gs_u16 = index_u16 ? 1 : 0;
    memory.GuardPhysicalRead(at, d.gs_bytes);
    const u8* const indices = memory.GetPhysicalPointer(at);
    DEBUG_ASSERT_MSG(indices != nullptr, "render thread: geometry draw indices at {:#x}", at);
    PutScanDraw(d, indices);
}

void RenderThread::ScanImmediateBatch() {
    // A batch keeps the decision its first vertex found; its holds come at its end.
    ListDraw d{};
    late_batch_skip = LateDraw(d, 0);
    late_batch_open = true;
    PutScanDraw(d, nullptr);
}

void RenderThread::ScanImmediateFlush() {
    const bool skip = late_batch_open && late_batch_skip;
    late_batch_open = false;
    if (skip) {
        return;
    }
    DrawTextures textures;
    const u32 count = TextureRanges(pica.regs.internal, textures);
    HoldDraw(textures, count, false, false);
}

void RenderThread::ScanImmediateVertex() {
    // RasterizerSgxd stops the emulator on the CPU path, with the draw's description.
    ListDraw d{};
    if (!LateDraw(d, 0)) {
        DrawTextures textures;
        const u32 count = TextureRanges(pica.regs.internal, textures);
        HoldDraw(textures, count, false, false);
    }
    PutScanDraw(d, nullptr);
}

void RenderThread::ScanIrq(u64 delay_ns) {
    DEBUG_ASSERT_MSG(scan_irqs != nullptr, "render thread: an interrupt request outside a scan");
    scan_irqs->push_back(delay_ns);
}

void RenderThread::PutScanDraw(const ListDraw& d, const u8* gs) {
    const std::size_t at = scan_draws.size();
    const u32 gs_room = (d.gs_bytes + 7) & ~7u;
    scan_draws.resize(at + sizeof(d) + gs_room);
    std::memcpy(scan_draws.data() + at, &d, sizeof(d));
    if (d.gs_bytes != 0) {
        std::memcpy(scan_draws.data() + at + sizeof(d), gs, d.gs_bytes);
    }
    scan_draw_count++;
    // A record at most a quarter of the ring (Begin): a list's draws in parts.
    if (scan_draws.size() >= RingSize / 16) {
        FlushScanDraws();
    }
}

void RenderThread::FlushScanDraws() {
    if (scan_draws.empty()) {
        return;
    }
    std::memcpy(Begin(Op::ListDraws, static_cast<u32>(scan_draws.size())), scan_draws.data(),
                scan_draws.size());
    Commit();
    scan_draws.clear();
}

void RenderThread::ReleaseParsed() {
    const u32 copied = copied_seq.load(std::memory_order_acquire);
    while (!parse_holds.empty() && static_cast<s32>(parse_holds.front().seq - copied) <= 0) {
        const ParseHold& h = parse_holds.front();
        for (const GuestRange& r : h.ranges) {
            page_holds.ReleaseParse(r.addr, r.bytes, h.released);
        }
        parse_holds.pop_front();
    }
}

bool RenderThread::ParseFault(u32 page) {
    ReleaseParsed();
    if (!page_holds.ParseHeld(page)) {
        // The pages the copied lists let go of open now (a guest store there must not abort on a page the
        // record says is open).
        page_holds.Apply();
        return false;
    }
    // Each unparsed list that reads the page reads a copy of it as handed over, and lets it go.
    std::shared_ptr<std::array<u8, 0x1000>> copy;
    bool parsing_reads = false;
    const u32 parsing = parsing_seq.load();
    for (ParseHold& h : parse_holds) {
        if (std::find(h.released.begin(), h.released.end(), page) != h.released.end()) {
            continue;
        }
        // A list and the buffers it jumped to may share the page: each held it.
        u32 covers = 0;
        for (const GuestRange& r : h.ranges) {
            u32 first, count;
            covers += page_holds.PageRange(r.addr, r.bytes, first, count) && page >= first &&
                      page < first + count;
        }
        if (covers == 0) {
            continue;
        }
        if (!copy) {
            copy = std::make_shared<std::array<u8, 0x1000>>();
            std::memcpy(copy->data(), page_holds.HostOf(page), copy->size());
#ifdef VITA_DIAGNOSTICS
            parse_copies++;
#endif
        }
        {
            std::scoped_lock lk{snapshot_mutex};
            snapshots.push_back({h.seq, page, copy});
        }
        snapshot_count.fetch_add(1);
        h.released.push_back(page);
        page_holds.ReleaseParsePage(page, covers);
        parsing_reads |= h.seq == parsing;
    }
    // The render side may be copying one of them from the guest's page now: the write waits. Seq-cst
    // against CopyListWords: this side adds a copy then reads `parsing_seq`, the render side stores
    // it then reads `snapshot_count`, so one of the two sees the other.
    const u32 now_parsing = parsing_seq.load();
    if (now_parsing != parsing) {
        parsing_reads = std::any_of(parse_holds.begin(), parse_holds.end(),
                                    [&](const ParseHold& h) {
                                        return h.seq == now_parsing &&
                                               std::find(h.released.begin(), h.released.end(),
                                                         page) != h.released.end();
                                    });
    }
    if (parsing_reads) {
#ifdef VITA_DIAGNOSTICS
        const auto start = Clock::now();
#endif
        std::scoped_lock lk{done};
        done_waiters.fetch_add(1);
        parse_waiting.store(true);
        while (static_cast<s32>(now_parsing - copied_seq.load()) > 0) {
            SleepDone(EmuSync);
        }
        parse_waiting.store(false);
        done_waiters.fetch_sub(1);
#ifdef VITA_DIAGNOSTICS
        parse_waits++;
        parse_wait_us += Since(start);
#endif
    }
    page_holds.Apply();
    return true;
}

void RenderThread::CopyListWords(u32 seq, const GuestRange* ranges, const u32* offsets,
                                 const u8* words, u32 count) {
    parse_ranges.clear();
    parse_words.clear();
    const bool copies = snapshot_count.load() != 0;
    std::unique_lock lk{snapshot_mutex, std::defer_lock};
    if (copies) {
        lk.lock();
    }
    const auto copy_of = [&](u32 page) -> const u8* {
        for (const Snapshot& s : snapshots) {
            if (s.seq == seq && s.page == page) {
                return s.bytes->data();
            }
        }
        return nullptr;
    };
    for (u32 i = 0; i < count; i++) {
        const GuestRange& r = ranges[i];
        const u32 at = static_cast<u32>(parse_words.size());
        parse_ranges.push_back({r.addr, r.bytes, at});
        parse_words.resize(at + r.bytes);
        if (offsets[i] != NotCopied) {
            std::memcpy(parse_words.data() + at, words + offsets[i], r.bytes);
            continue;
        }
        const u8* const guest = memory.GetPhysicalPointer(r.addr);
        if (!copies) {
            std::memcpy(parse_words.data() + at, guest, r.bytes);
            continue;
        }
        for (u32 done = 0; done < r.bytes;) {
            const u8* const p = guest + done;
            const u32 in_page = 0x1000 - static_cast<u32>(reinterpret_cast<uintptr_t>(p) & 0xFFF);
            const u32 n = std::min(in_page, r.bytes - done);
            const u8* const page_copy = copy_of(page_holds.PageOf(p));
            std::memcpy(parse_words.data() + at + done,
                        page_copy ? page_copy + (reinterpret_cast<uintptr_t>(p) & 0xFFF) : p, n);
            done += n;
        }
    }
    if (copies) {
        const auto done_with = [&](const Snapshot& s) {
            return static_cast<s32>(s.seq - seq) <= 0;
        };
        const u32 dropped =
            static_cast<u32>(std::count_if(snapshots.begin(), snapshots.end(), done_with));
        std::erase_if(snapshots, done_with);
        snapshot_count.fetch_sub(dropped);
    }
}

const u8* RenderThread::RenderList(PAddr addr, u32 size) {
    DEBUG_ASSERT_MSG(parse_range_at < parse_ranges.size() &&
                         parse_ranges[parse_range_at][0] == addr &&
                         parse_ranges[parse_range_at][1] == size,
                     "render thread: list {}: the parse reads {:#010x}+{:#x}, the scan's range {} "
                     "of {} differs",
                     parsing_seq.load(std::memory_order_relaxed), addr, size, parse_range_at,
                     parse_ranges.size());
    return parse_words.data() + parse_ranges[parse_range_at++][2];
}

void RenderThread::RenderFogLut() {
    // The parsed Op::Fog record (SyncParsedState) tells the rasterizer, at its draw.
}

bool RenderThread::RenderDraw() {
    DEBUG_ASSERT_MSG(parse_draw_at + sizeof(ListDraw) <= parse_draws.size(),
                     "render thread: list {}: a draw the scan did not see",
                     parsing_seq.load(std::memory_order_relaxed));
    ListDraw d;
    std::memcpy(&d, parse_draws.data() + parse_draw_at, sizeof(d));
    const u8* const gs = parse_draws.data() + parse_draw_at + sizeof(d);
    parse_draw_at += sizeof(d) + ((d.gs_bytes + 7) & ~7u);
    if (d.report != 0) {
        ParsedPut(Op::LateDropped, d.late_report);
    }
    if (d.dropped != 0) {
        return false;
    }
    parse_indices = d.indices;
    parse_gs = d.gs_bytes != 0 ? gs : nullptr;
    parse_gs_bytes = d.gs_bytes;
    parse_gs_u16 = d.gs_u16 != 0;
    return true;
}

void RenderThread::PublishParsed() {
    // Sequentially consistent against the render side's `sleeping` and `done_waiters`.
    parsed_published.store(parsed_write);
    if (sleeping.load()) {
        Wake();
    }
    if (done_waiters.load() != 0) {
        NotifyDone();
    }
}

void RenderThread::ParseRecord(const Head& head, const u8* p) {
    switch (head.op) {
    case Op::ListDraws:
        parse_draws.insert(parse_draws.end(), p, p + (head.bytes - sizeof(Head)));
        break;
    case Op::RegWrite: {
        std::array<u32, 2> w;
        std::memcpy(&w, p, sizeof(w));
        parse_pica.regs.internal.reg_array[w[0]] = w[1];
        parse_pica.dirty_regs.Set(w[0]);
        break;
    }
    case Op::CmdList: {
        const TraceSpan span{"parse", "parse_end"};
        CmdListRecord c;
        std::memcpy(&c, p, sizeof(c));
        // The words first, whole (CopyListWords, ParseFault); then the list's holds can go, and a guest
        // write never waits for the parse.
        parsing_seq.store(c.seq);
        static_assert(sizeof(CmdListRecord) % alignof(GuestRange) == 0);
        const u32 ranges_bytes = c.ranges * static_cast<u32>(sizeof(GuestRange));
        const u8* const offsets = p + sizeof(c) + ranges_bytes;
        CopyListWords(c.seq, reinterpret_cast<const GuestRange*>(p + sizeof(c)),
                      reinterpret_cast<const u32*>(offsets),
                      offsets + Align8(c.ranges * static_cast<u32>(sizeof(u32))), c.ranges);
        // Seq-cst against ParseFault's wait: either it sees the count, or this sees it waiting.
        copied_seq.store(c.seq);
        if (parse_waiting.load()) {
            NotifyDone();
        }
        parse_draw_at = 0;
        parse_range_at = 0;
        parse_pica.ProcessCmdList(c.addr, c.size, false);
        DEBUG_ASSERT_MSG(parse_draw_at == parse_draws.size(),
                         "render thread: list {} at {:#010x}: {} bytes of the scan's {} draw "
                         "decisions left after its parse",
                         c.seq, c.addr, parse_draws.size() - parse_draw_at, c.draws);
        DEBUG_ASSERT_MSG(parse_range_at == parse_ranges.size(),
                         "render thread: list {} at {:#010x}: the parse read {} of the scan's {} "
                         "ranges",
                         c.seq, c.addr, parse_range_at, parse_ranges.size());
        parse_draws.clear();
        // The state the list left (its last draws' and after) goes with its end.
        SyncParsedState();
        ParsedPut(Op::ListEnd, c.seq);
        PublishParsed();
        break;
    }
    default:
        break;
    }
}

void RenderThread::ParseLoop() {
    Common::SetCurrentThreadRole(Common::ThreadRole::Parse);
    for (;;) {
        u32 r = parse_read.load(std::memory_order_relaxed);
        if (r == published.load()) {
            if (parse_stopping.load()) {
                return;
            }
            if (done_waiters.load() != 0) {
                NotifyDone(); // WaitParseIdle
            }
            // Seq-cst against Publish: it sees this sleeping, or this sees its record.
            parse_sleeping.store(true);
            if (r == published.load() && !parse_stopping.load()) {
                ParseSleep(std::chrono::milliseconds(100));
            }
            parse_sleeping.store(false);
            continue;
        }
        const u32 end = published.load(std::memory_order_acquire);
#ifdef VITA_DIAGNOSTICS
        const auto start = Clock::now();
#endif
        while (r != end) {
            Head head;
            const u8* const p = ring.get() + r % RingSize;
            std::memcpy(&head, p, sizeof(head));
            if (head.op != Op::Wrap) {
                ParseRecord(head, p + sizeof(Head));
            }
            r += head.bytes;
            parse_read.store(r, std::memory_order_release);
            if (space_waiting.load()) {
                NotifyDone();
            }
        }
#ifdef VITA_DIAGNOSTICS
        parse_busy_us.fetch_add(Since(start), std::memory_order_relaxed);
#endif
    }
}

bool RenderThread::ParseSleep(std::chrono::microseconds timeout) {
    SceUInt32 us = static_cast<SceUInt32>(timeout.count());
    return sceKernelWaitEventFlag(parse_wake_flag, WakeBit,
                                  SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, nullptr, &us) >= 0;
}

void RenderThread::ParseWake() {
#ifdef VITA_DIAGNOSTICS
    const auto start = Clock::now();
#endif
    sceKernelSetEventFlag(parse_wake_flag, WakeBit);
#ifdef VITA_DIAGNOSTICS
    parse_wakes.fetch_add(1, std::memory_order_relaxed);
    wake_us.fetch_add(Since(start), std::memory_order_relaxed);
#endif
}

void RenderThread::WaitParseIdle() {
    std::scoped_lock lk{done};
    done_waiters.fetch_add(1);
    while (parse_read.load() != published.load()) {
        ParseWake();
        done.WaitFor(std::chrono::milliseconds(1));
    }
    done_waiters.fetch_sub(1);
}

void RenderThread::RunParsed(u32 seq) {
    const TraceSpan span{"list_run", "list_run_end"};
    // On this thread, or a hold's: the parse thread runs either way.
    const bool render_side = sceKernelGetThreadId() == render_tid.load(std::memory_order_relaxed);
    u32 since_poll = 0;
    for (;;) {
        u32 r = parsed_read.load(std::memory_order_relaxed);
        if (r == parsed_published.load()) {
#ifdef VITA_DIAGNOSTICS
            const auto start = Clock::now();
            parsed_waits.fetch_add(1, std::memory_order_relaxed);
#endif
            if (render_side) {
                const TraceSpan wait_span{"parsed_wait", "parsed_wait_end"};
                // Sequentially consistent against PublishParsed.
                sleeping.store(true);
                if (r == parsed_published.load()) {
                    Sleep(std::chrono::milliseconds(100));
                }
                sleeping.store(false);
            } else {
                std::scoped_lock lk{done};
                done_waiters.fetch_add(1);
                if (r == parsed_published.load()) {
                    done.WaitFor(std::chrono::milliseconds(1));
                }
                done_waiters.fetch_sub(1);
            }
#ifdef VITA_DIAGNOSTICS
            parsed_wait_us.fetch_add(Since(start), std::memory_order_relaxed);
#endif
            continue;
        }
        Head head;
        const u8* const p = parsed.get() + r % ParsedSize;
        std::memcpy(&head, p, sizeof(head));
        bool end = false;
        if (head.op == Op::ListEnd) {
            u32 ended;
            std::memcpy(&ended, p + sizeof(Head), sizeof(ended));
            DEBUG_ASSERT_MSG(ended == seq, "render thread: list {}'s parsed records end list {}",
                             seq, ended);
            end = true;
        } else if (head.op != Op::Wrap) {
            RunRecord(head, p + sizeof(Head));
#ifdef VITA_DIAGNOSTICS
            records[KindOf(head.op)].fetch_add(1, std::memory_order_relaxed);
#endif
        }
        parsed_read.store(r + head.bytes, std::memory_order_release);
        if (parsed_space_waiting.load()) {
            ParseWake();
        }
        if (end) {
            return;
        }
        // The loop's work between records, inside a list too (a list's records can run tens of ms, the
        // tickets its GPU work finished meanwhile unpublished). Every 16 records, without sending a
        // pending 3D: the pass may still go on.
        if ((++since_poll & 15) == 0 && !pending.empty()) {
            bool advanced = false;
            while (!pending.empty() && rasterizer.InterruptReached(pending.front().fence)) {
                reached.store(pending.front().ticket, std::memory_order_release);
                SgxdTrace("reached", static_cast<unsigned>(pending.front().ticket));
                pending.pop_front();
                advanced = true;
            }
            if (advanced) {
                NotifyDone();
            }
        }
        if (render_side) {
            if ((since_poll & 63) == 0) {
                renderer.FlipDone();
            }
            if (present_pending && (since_poll & 255) == 0) {
                ShowDuePresent();
            }
        }
    }
}

void RenderThread::GpuIdle() {
    Put(Op::GpuIdle);
}

u64 RenderThread::InterruptFence() {
    const u32 ticket = ++tickets;
    page_holds.Close(ticket);
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

void RenderThread::WaitForGpuWrites() {
    if (last_write_ticket != 0 && !InterruptPoll(last_write_ticket)) {
        if (last_write_ticket == HoldTicket()) {
            InterruptFence();
        }
        InterruptWait(last_write_ticket);
    }
}

void RenderThread::WaitGpuWrites(PAddr addr, u32 size) {
    // A page move's copy (GuestGpuMemory): the GPU's writes queued to the pages land first.
    // Before the native core turns the holds on there is no page's ticket: the last write's.
    if (!page_holds_on) {
        WaitForGpuWrites();
        return;
    }
    for (PAddr at = addr & ~0xfffu; at < addr + size; at += 0x1000) {
        const u32 page = page_holds.PageOf(memory.GetPhysicalPointer(at));
        if (page == PageHolds::NoPage) {
            continue;
        }
        const u32 ticket = page_holds.WriteTicket(page);
        if (InterruptPoll(ticket)) {
            continue;
        }
        // A write hold made since the last fence has no ticket yet: it gets one now.
        if (ticket == HoldTicket()) {
            InterruptFence();
        }
        InterruptWait(ticket);
    }
}

void RenderThread::EnablePageHolds() {
    page_holds_on = true;
}

void RenderThread::ApplyPageHolds() {
    page_holds.Apply();
}

void RenderThread::ReleasePageHolds() {
    ReleaseParsed();
    page_holds.Release(reached.load(std::memory_order_acquire));
}

void RenderThread::WaitHeldPage(u32 page, bool write) {
    const u32 ticket = page_holds.WaitFor(page, write, reached.load(std::memory_order_acquire));
    if (ticket == 0) {
        return;
    }
    // The holds made since the last fence have no ticket yet: they get one now.
    if (ticket == HoldTicket()) {
        ASSERT_MSG(sceKernelGetThreadId() == producer,
                   "render thread: a fence for a held page queued off the emulation thread; {}",
                   Common::DescribeWaiters(wake_flag));
        InterruptFence();
    }
    InterruptWait(ticket);
}

bool RenderThread::ReadMissesWrites(const u8* host, u32 size, u32& write_ticket) {
    if (!page_holds_on) {
        return false;
    }
    const u32 page = page_holds.PageOf(host);
    if (page == PageHolds::NoPage || page_holds.PageOf(host + size - 1) != page ||
        page_holds.Applied(page) != PageHolds::NoAccess) {
        return false;
    }
    // The page's GPU write done (holds are let go of lazily): the fault's path opens it without a
    // wait, and the guest's next loads there run natively. Not a page a picture's rows close.
    const PAddr page_addr = page_holds.AddressOf(page);
    if (page_holds.WaitFor(page, false, reached.load(std::memory_order_acquire)) == 0 &&
        !rasterizer.PictureOwns(page_addr, page_addr + 0x1000)) {
        return false;
    }
    write_ticket = page_holds.WriteTicket(page);
    return !page_holds.TouchesWrite(host, size);
}

bool RenderThread::StoreMissesHolds(const u8* host, u32 size, u32& ticket) {
    if (!page_holds_on) {
        return false;
    }
    const u32 page = page_holds.PageOf(host);
    if (page == PageHolds::NoPage || page_holds.PageOf(host + size - 1) != page ||
        page_holds.Applied(page) == PageHolds::Unheld) {
        return false;
    }
    // A store to a page of the memo's index lists: a write, as PageFault takes it; past the holds
    // then as any store. Closed by the watch alone, the page is PageFault's to open: opened here,
    // PageFault would find no hold over it and take the abort for the guest's own (a game's
    // command-buffer page).
    if (page_holds.IndexWatched(page)) {
        if (page_holds.ClosedByIndexWatchAlone(page, reached.load(std::memory_order_acquire))) {
            return false;
        }
        page_holds.IndexWritten(page, reached.load(std::memory_order_acquire));
    }
    return page_holds.StoreMisses(host, size, reached.load(std::memory_order_acquire), ticket);
}

void RenderThread::StoreForbidden(const u8* host, std::array<u16, 6>& lo, std::array<u16, 6>& hi,
                                  u32 slot) {
    page_holds.StoreForbidden(host, reached.load(std::memory_order_acquire), lo, hi);
    page_holds.PermitGranted(host, slot);
}

bool RenderThread::OpensDisplayPage(const u8* host, bool write, u32 reach) {
    if (!page_holds_on) {
        return false;
    }
    const u32 page = page_holds.PageOf(host);
    if (page == PageHolds::NoPage) {
        return false;
    }
    const PageHolds::Access access = page_holds.Applied(page);
    if (access == PageHolds::Unheld || (access == PageHolds::ReadOnly && !write)) {
        return false;
    }
    return page_holds.OpenPastDisplay(host, now, reach);
}

bool RenderThread::AccessOpenAfterFlush(const u8* host, bool write) {
    if (!page_holds_on) {
        return false;
    }
    const u32 page = page_holds.PageOf(host);
    if (page == PageHolds::NoPage) {
        return false;
    }
    // The record changes first (PageHolds::Change), the descriptors when they are sent.
    page_holds.Apply();
    const PageHolds::Access access = page_holds.Applied(page);
    return access == PageHolds::Unheld || (access == PageHolds::ReadOnly && !write);
}

bool RenderThread::PageFault(const u8* host, bool write, [[maybe_unused]] u32 guest_pc,
                             [[maybe_unused]] u32 guest_va) {
    if (!page_holds_on) {
        return false;
    }
    const u32 page = page_holds.PageOf(host);
    if (page == PageHolds::NoPage) {
        return false;
    }
    // A hold refuses writes to a page the GPU reads, and any access to one it writes.
    const PageHolds::Access access = page_holds.Applied(page);
    if (access == PageHolds::Unheld || (access == PageHolds::ReadOnly && !write)) {
        return false;
    }
    // A write to a page whose index lists the scan's memo holds (HoldVertices): the memo's
    // ranges there are stale, and the page opens as its other holds allow.
    if (write && page_holds.IndexWatched(page)) {
        page_holds.IndexWritten(page, reached.load(std::memory_order_acquire));
        if (page_holds.Applied(page) == PageHolds::Unheld) {
            return true;
        }
    }
    // A write to a page a handed-over list's parse reads: that parse reads a copy of it
    // (ParseFault), and the page opens as its other holds allow.
    if (write && ParseFault(page) && page_holds.Applied(page) == PageHolds::Unheld) {
        return true;
    }
    // T12: a write to a texture's page: the textures over it are untiled again at their next
    // use, the page open to writes as its holds allow.
    if (write && page_holds.Watched(page)) {
        TexturesWritten(page_holds.AddressOf(page), 0x1000);
        page_holds.Unwatch(page, reached.load(std::memory_order_acquire));
#ifdef VITA_DIAGNOSTICS
        texture_write_faults++;
#endif
        if (page_holds.Applied(page) == PageHolds::Unheld) {
            return true;
        }
    }
    // A write to an FCRAM page the GPU still reads takes a version of it (moved pages only:
    // their home in CDRAM needs no cache work), and the guest goes on at once.
    const PAddr address = page_holds.AddressOf(page);
    if (write && address >= Memory::FCRAM_PADDR && address < Memory::FCRAM_PADDR_END &&
        !renderer.WouldMakeUncached(address, 0x1000)) {
        if (page_holds.Version(page, reached.load(std::memory_order_acquire))) {
#ifdef VITA_DIAGNOSTICS
            faults++;
            versioned_faults++;
#endif
            return true;
        }
    }
    // A write to a cached FCRAM page that only queued draws' geometry reads holds (a buffer the guest
    // refills while the GPU still draws it): the page moves to CDRAM after this wait, so the next write
    // takes a version instead.
    const bool move_after = write && !GeometryStaysCached() && address >= Memory::FCRAM_PADDR &&
                            address < Memory::FCRAM_PADDR_END &&
                            page_holds.GeometryReadOnly(page, reached.load(std::memory_order_acquire)) &&
                            renderer.WouldMakeUncached(address, 0x1000);
#ifdef VITA_DIAGNOSTICS
    faults++;
    const auto start = Clock::now();
    const u32 before = reached.load(std::memory_order_acquire);
    const u32 waited = page_holds.WaitFor(page, write, before);
    const PageHolds::Kind kind = page_holds.WaitKind(page, write, before);
#endif
    WaitHeldPage(page, write);
#ifdef VITA_DIAGNOSTICS
    if (waited != 0) {
        const u64 us = Since(start);
        fault_waits++;
        fault_wait_us += us;
        fault_kinds[kind]++;
        fault_kind_us[kind] += us;
        auto& site = fault_sites[(static_cast<u64>(guest_pc) << 8) | kind];
        site.count++;
        site.us += us;
        site.page = page_holds.AddressOf(page);
        static const bool holdlog = SgxdFlag("holdlog") || SgxdFlagValue("holdlog") != 0;
        static const u64 holdlog_from_us = u64{SgxdFlagValue("holdlog")} * 1000000;
        static u32 logged = 0;
        if (holdlog &&
            static_cast<u64>(Clock::now().time_since_epoch().count()) >= holdlog_from_us &&
            logged++ < 200) {
            const auto [range_addr, range_bytes] = page_holds.RangeOf(page);
            const auto [writer, write_range] = page_holds.WriterOf(page);
            LOG_INFO(Render,
                     "holdlog: guest {} at {:#010x} (pc {:#010x}, {:08x}) on {:#010x}+{:#x}: {} "
                     "hold of {:#010x}+{:#x}, ticket {} with {} reached and {} queued, waited {} "
                     "us; last write {} of {:#010x}+{:#x}; {}",
                     write ? "write" : "read", guest_va, guest_pc, memory.Read32(guest_pc),
                     page_holds.AddressOf(page),
                     guest_va & 0xfff, PageHolds::KindName(kind), range_addr, range_bytes, waited,
                     before, tickets, us, PageHolds::KindName(writer), write_range.first,
                     write_range.second, page_holds.DisplayState(page, before));
        }
    }
#endif
    page_holds.Reopen(page, reached.load(std::memory_order_acquire));
    [[maybe_unused]] const PageHolds::Access reopened = page_holds.Applied(page);
    if (move_after) {
        Hold();
        renderer.MakeUncached(address, 0x1000, false);
        Release();
#ifdef VITA_DIAGNOSTICS
        geometry_moves++;
#endif
    }
    [[maybe_unused]] const PageHolds::Access after_move = page_holds.Applied(page);
    // A versioned page the frame closed to writes, written while a GPU write was queued: with that
    // write done the version is taken now.
    if (write && page_holds.Applied(page) != PageHolds::Unheld && address >= Memory::FCRAM_PADDR &&
        address < Memory::FCRAM_PADDR_END && !renderer.WouldMakeUncached(address, 0x1000)) {
        page_holds.Version(page, reached.load(std::memory_order_acquire));
    }
    DEBUG_ASSERT_MSG(page_holds.Applied(page) == PageHolds::Unheld ||
                         (page_holds.Applied(page) == PageHolds::ReadOnly && !write),
                     "render thread: page {} ({:#010x}) still held after its work ({} access): "
                     "{} after the reopen, {} after the move{}; texture watch {}, index watch "
                     "{}; {}; {}",
                     page, address, write ? "a write" : "a read", static_cast<u32>(reopened),
                     static_cast<u32>(after_move), move_after ? " (moved)" : "",
                     page_holds.Watched(page), page_holds.IndexWatched(page),
                     page_holds.HoldState(page, reached.load(std::memory_order_acquire)),
                     page_holds.DisplayState(page, reached.load(std::memory_order_acquire)));
    return true;
}

void RenderThread::HostAccess(const u8* host, u32 size, bool write) {
    const u8* const end = host + size;
    for (const u8* p = host; p < end;
         p = reinterpret_cast<const u8*>((reinterpret_cast<uintptr_t>(p) | 0xFFF) + 1)) {
        const u32 page = page_holds.PageOf(p);
        if (page == PageHolds::NoPage || page_holds.Applied(page) == PageHolds::Unheld) {
            continue;
        }
        // The HLE writes a page of the memo's index lists, or one a handed-over list's parse
        // reads: as a guest write does.
        if (write && page_holds.IndexWatched(page)) {
            page_holds.IndexWritten(page, reached.load(std::memory_order_acquire));
            if (page_holds.Applied(page) == PageHolds::Unheld) {
                continue;
            }
        }
        if (write && ParseFault(page) && page_holds.Applied(page) == PageHolds::Unheld) {
            continue;
        }
        // T12: the HLE writes a texture's page (a file read, the DSP): as a guest write does.
        if (write && page_holds.Watched(page)) {
            TexturesWritten(page_holds.AddressOf(page), 0x1000);
            page_holds.Unwatch(page, reached.load(std::memory_order_acquire));
#ifdef VITA_DIAGNOSTICS
            texture_write_hle++;
#endif
        }
        // A read clear of the bytes the queued GPU writes cover goes on: the page shares them
        // with other data (PageHolds::TouchesWrite). The HLE's reads do not change the guest's
        // view, which stays held.
        const u8* const page_end =
            reinterpret_cast<const u8*>((reinterpret_cast<uintptr_t>(p) | 0xFFF) + 1);
        const u32 bytes = static_cast<u32>(std::min(end, page_end) - p);
        if (!write && !page_holds.TouchesWrite(p, bytes)) {
            continue;
        }
#ifdef VITA_DIAGNOSTICS
        const auto start = Clock::now();
        const u32 before = reached.load(std::memory_order_acquire);
        const u32 waited = page_holds.WaitFor(page, write, before);
        const auto [writer, writer_range] = page_holds.WriterOf(page);
#endif
        WaitHeldPage(page, write);
#ifdef VITA_DIAGNOSTICS
        if (reached.load(std::memory_order_acquire) != before) {
            const u64 us = Since(start);
            host_waits++;
            host_wait_us += us;
            static const bool holdlog = SgxdFlag("holdlog") || SgxdFlagValue("holdlog") != 0;
            static const u64 holdlog_from_us = u64{SgxdFlagValue("holdlog")} * 1000000;
            static u32 logged = 0;
            if (holdlog &&
                static_cast<u64>(Clock::now().time_since_epoch().count()) >= holdlog_from_us &&
                logged++ < 200) {
                LOG_INFO(Render,
                         "holdlog: HLE {} of {:#010x}+{:#x}: {} write of {:#010x}+{:#x}, ticket "
                         "{} with {} reached and {} queued, waited {} us",
                         write ? "write" : "read",
                         page_holds.AddressOf(page) + (reinterpret_cast<uintptr_t>(p) & 0xfff),
                         bytes, PageHolds::KindName(writer), writer_range.first,
                         writer_range.second, waited, before, tickets, us);
            }
        }
#endif
    }
}

void RenderThread::InvalidateWritten(PAddr addr, u32 size) {
    if (!page_holds_on || size == 0) {
        return;
    }
    // Y2R writes its picture through GetPointer after this (a game's Grand Prix preview video):
    // without it the textures over those pages kept their first frame, and the video went
    // back and forth between it and the next one.
    const u8* const host = memory.GetPhysicalPointer(addr);
    if (host == nullptr) {
        return;
    }
    const u8* const end = host + size;
    // The range's FCRAM pages, first and one past the last.
    u32 first = PageHolds::NoPage, last = 0;
    for (const u8* p = host; p < end;
         p = reinterpret_cast<const u8*>((reinterpret_cast<uintptr_t>(p) | 0xFFF) + 1)) {
        const u32 page = page_holds.PageOf(p);
        if (page != PageHolds::NoPage && page < Memory::FCRAM_SIZE >> 12) {
            // Watched or not: Y2R writes without a fault. A watched texture page opens as its
            // other holds allow; the range's record below marks its textures (T12).
            page_holds.IndexWrittenUnsent(page, reached.load(std::memory_order_acquire));
            if (page_holds.Watched(page)) {
                page_holds.UnwatchUnsent(page, reached.load(std::memory_order_acquire));
            }
            first = std::min(first, page);
            last = std::max(last, page + 1);
#ifdef VITA_DIAGNOSTICS
            texture_write_hle++;
#endif
        }
    }
    if (first != PageHolds::NoPage) {
        TexturesWritten(page_holds.AddressOf(first), (last - first) << 12);
    }
    // The guest's view of the range in one azaharHold. Sent page by page, each was a syscall
    // forwarded out of the upcall (Y2R's StartConversion): MH Generations' opening movie, ~70
    // pages a conversion, made ~2000 a second at ~25000 cycles each.
    page_holds.Apply();
}

void RenderThread::TexturesWritten(PAddr address, u32 size) {
    ASSERT_MSG(sceKernelGetThreadId() == producer,
               "render thread: a texture's page written off the emulation thread; {}",
               Common::DescribeWaiters(wake_flag));
    // Closed, not published: the texture's next use is a later record, whose publish takes this along
    // (publishing would wake a sleeping render thread each time).
    const std::array<u32, 2> range{static_cast<u32>(address), size};
    std::memcpy(Begin(Op::GuestWrote, sizeof(range)), range.data(), sizeof(range));
    write += open_record;
    ForgetDroppedOutputs(address, address + size);
}

#ifdef VITA_DIAGNOSTICS
void RenderThread::CheckHoldsSent(const char* where) {
    if (!page_holds_on) {
        return;
    }
    const u32 page = page_holds.LoweredUnsent();
    DEBUG_ASSERT_MSG(page == PageHolds::NoPage,
                     "render thread: page {:#010x} let go of but not sent as {} goes on: {}",
                     page_holds.AddressOf(page), where,
                     page_holds.HoldState(page, reached.load(std::memory_order_acquire)));
}

std::string RenderThread::HoldState(const u8* host) {
    const u32 page = page_holds.PageOf(host);
    if (!page_holds_on || page == PageHolds::NoPage) {
        return "no page of the holds";
    }
    return fmt::format("page {:#010x}: {}; {}", page_holds.AddressOf(page),
                       page_holds.HoldState(page, reached.load(std::memory_order_acquire)),
                       renderer.DescribePages(page_holds.AddressOf(page), 0x1000));
}

std::string RenderThread::VersionList() const {
    return page_holds_on ? page_holds.VersionList() : "the page holds are off";
}

std::pair<u32, u32> RenderThread::TakeTextureWrites() {
    return {std::exchange(texture_write_faults, 0), std::exchange(texture_write_hle, 0)};
}
#endif

void RenderThread::HostRemapped(const u8* host, u32 size) {
    if (page_holds_on) {
        page_holds.Resend(host, size);
    }
}

#ifdef VITA_DIAGNOSTICS
RenderThread::HoldStats RenderThread::TakeHoldStats() {
    HoldStats h{};
    h.holds = page_holds.TakeStats();
    h.faults = std::exchange(faults, 0);
    h.fault_waits = std::exchange(fault_waits, 0);
    h.fault_wait_us = std::exchange(fault_wait_us, 0);
    h.host_waits = std::exchange(host_waits, 0);
    h.host_wait_us = std::exchange(host_wait_us, 0);
    h.versioned_faults = std::exchange(versioned_faults, 0);
    h.geometry_moves = std::exchange(geometry_moves, 0);
    h.parse_copies = std::exchange(parse_copies, 0);
    h.parse_waits = std::exchange(parse_waits, 0);
    h.parse_wait_us = std::exchange(parse_wait_us, 0);
    h.fault_kinds = std::exchange(fault_kinds, {});
    h.fault_kind_us = std::exchange(fault_kind_us, {});
    std::vector<std::pair<u64, FaultSite>> sites(fault_sites.begin(), fault_sites.end());
    fault_sites.clear();
    std::sort(sites.begin(), sites.end(),
              [](const auto& a, const auto& b) { return a.second.us > b.second.us; });
    h.site_count = static_cast<u32>(std::min<std::size_t>(sites.size(), h.sites.size()));
    for (u32 i = 0; i < h.site_count; i++) {
        h.sites[i] = {static_cast<u32>(sites[i].first >> 8),
                      static_cast<PageHolds::Kind>(sites[i].first & 0xff), sites[i].second};
    }
    return h;
}
#endif

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

void RenderThread::Present(const std::array<Pica::FramebufferConfig, 2>& guest_fb,
                           const std::array<Pica::ColorFill, 2>& fill) {
    std::array<Pica::FramebufferConfig, 2> fb = guest_fb;
    lcd_fb = fb;
    late_generation++;
    // Each buffer a present names is noted with the present; one no present named in the last eight is
    // forgotten (a title's memory reuses buffers it showed before).
    presents_named++;
    const auto note = [this](std::vector<LcdBuffer>& known, PAddr a) {
        const auto it = std::find_if(known.begin(), known.end(),
                                     [a](const LcdBuffer& b) { return b.addr == a; });
        if (it != known.end()) {
            it->named = presents_named;
            return;
        }
        if (known.size() == 4) {
            known.erase(known.begin());
        }
        known.push_back({a, 0, presents_named});
    };
    const auto forget = [this](std::vector<LcdBuffer>& known) {
        std::erase_if(known, [this](const LcdBuffer& b) { return presents_named - b.named > 8; });
    };
    // The top screen's right-eye buffers apart from its left ones (a stereo title's).
    for (const PAddr a :
         {static_cast<PAddr>(fb[0].address_right1), static_cast<PAddr>(fb[0].address_right2)}) {
        if (a != 0 && a != fb[0].address_left1 && a != fb[0].address_left2) {
            note(right_eye_buffers, a);
        }
    }
    forget(right_eye_buffers);
    // Every buffer a screen has been shown from (a title may render into a third one the LCD's two
    // registers do not name yet).
    for (u32 i = 0; i < 2; i++) {
        for (const PAddr a :
             {static_cast<PAddr>(fb[i].address_left1), static_cast<PAddr>(fb[i].address_left2)}) {
            if (a != 0) {
                note(lcd_buffers[i], a);
            }
        }
        forget(lcd_buffers[i]);
    }
    // The late skip: a screen whose selected LCD buffer holds a picture older than the one it last
    // showed shows the oldest picture no older than that instead. A buffer the GPU never wrote is shown
    // as it is (the CPU's picture, no stamp).
    for (u32 i = 0; i < 2; i++) {
        const PAddr selected = fb[i].second_fb_active ? fb[i].address_left2 : fb[i].address_left1;
        auto& buffers = lcd_buffers[i];
        const auto it = std::find_if(buffers.begin(), buffers.end(),
                                     [selected](const LcdBuffer& b) { return b.addr == selected; });
        if (it == buffers.end() || it->stamp >= shown_stamp[i] || !it->gpu) {
#ifdef VITA_DIAGNOSTICS
            if (screenlog_on) {
                LOG_INFO(Render, "screenlog frame {}: screen {} shows selected {:#x} (stamp {}{})",
                         screenlog_frame, i, selected, it == buffers.end() ? 0 : it->stamp,
                         it == buffers.end() ? ", unknown" : "");
            }
#endif
            shown_stamp[i] = it == buffers.end() ? shown_stamp[i] : it->stamp;
            continue;
        }
        // The oldest picture no older than the last shown: the screen follows the guest's buffers again
        // once they catch up.
        LcdBuffer* next = nullptr;
        for (LcdBuffer& b : buffers) {
            if (b.stamp >= shown_stamp[i] && (!next || b.stamp < next->stamp)) {
                next = &b;
            }
        }
        DEBUG_ASSERT_MSG(next, "late skip: no LCD buffer of screen {} holds stamp {} or newer", i,
                         shown_stamp[i]);
#ifdef VITA_DIAGNOSTICS
        if (screenlog_on) {
            LOG_INFO(Render,
                     "screenlog frame {}: screen {} selected {:#x} (stamp {}), older than shown "
                     "{}: shows {:#x} (stamp {})",
                     screenlog_frame, i, selected, it->stamp, shown_stamp[i], next->addr,
                     next->stamp);
        }
#endif
        fb[i].address_left1 = next->addr;
        fb[i].address_left2 = next->addr;
        shown_stamp[i] = next->stamp;
        // Shown, it is named (kept while the screen waits on it).
        next->named = presents_named;
#ifdef VITA_DIAGNOSTICS
        late_partial.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    // The present reads the screens it shows (its picture is drawn from them on the GPU).
    for (u32 i = 0; i < 2; i++) {
        const Pica::FramebufferConfig& f = fb[i];
        HoldRead(f.second_fb_active ? f.address_left2 : f.address_left1,
                 f.stride * (i == 0 ? 400u : 320u), PageHolds::Present);
    }
    const PresentRecord record{fb, fill};
    std::memcpy(Begin(Op::Present, sizeof(record)), &record, sizeof(record));
    // Counted before it is published, so that the thread never sees the record uncounted.
    presents_queued.fetch_add(1, std::memory_order_relaxed);
    Commit();
}

void RenderThread::FrameTicket(u64 ticket) {
    frame_ticket_at = (frame_ticket_at + 1) % LeadFramesBack;
    frame_tickets[frame_ticket_at] = ticket;
    SgxdTrace("frame_ticket", static_cast<unsigned>(ticket));
}

void RenderThread::EndFrame() {
#ifdef VITA_DIAGNOSTICS
    if (screenlog_on) {
        std::string targets, map;
        for (const ScreenlogTarget& t : screenlog_targets) {
            targets += fmt::format(" {:#x}+{:#x}:s{} {}d/{}x", t.addr, t.bytes, t.screen, t.draws,
                                   t.dropped);
        }
        for (const SegmentTarget& t : segment_targets) {
            u32 used = SegmentSlots;
            while (used > 1 && t.screens[used - 1] < 0) {
                used--;
            }
            map += fmt::format(" {:#x}+{:#x}:s{}", t.addr, t.bytes,
                               fmt::join(t.screens.begin(), t.screens.begin() + used, "/"));
        }
        LOG_INFO(
            Render,
            "screenlog frame {}: late {} keep {} sums {}/{}; LCD top {:#x}/{:#x} right "
            "{:#x}/{:#x} bottom {:#x}/{:#x}; drawn:{}; transfers:{}; map:{}",
            screenlog_frame, late_skip, late_keep, screen_cost[0], screen_cost[1],
            static_cast<u32>(lcd_fb[0].address_left1), static_cast<u32>(lcd_fb[0].address_left2),
            static_cast<u32>(lcd_fb[0].address_right1), static_cast<u32>(lcd_fb[0].address_right2),
            static_cast<u32>(lcd_fb[1].address_left1), static_cast<u32>(lcd_fb[1].address_left2),
            targets, screenlog_transfers, map);
        screenlog_targets.clear();
        screenlog_transfers.clear();
    }
    static const bool screenlog = SgxdFlag("screenlog");
    static const u64 screenlog_from_us = u64{SgxdFlagValue("screenlog_at")} * 1000000;
    static u32 screenlog_window = 0;
    const bool window = screenlog_from_us != 0 &&
                        static_cast<u64>(Clock::now().time_since_epoch().count()) >=
                            screenlog_from_us &&
                        screenlog_window < 600;
    screenlog_window += window ? 1 : 0;
    screenlog_frame++;
    screenlog_on = window || (screenlog && screenlog_frame % 600 < 4);
#endif
    // The late skip, decided where the draws are recorded so a late frame's dropped draws and
    // transfers never are. The next frame is late when the guest's frame TicketFramesBack vblanks back
    // is not done. A late frame drops the busier screen's draws (each draw's screen its buffer's
    // segment's), the draws of no known screen, and the transfers ending a segment with a dropped draw.
    for (SegmentTarget& t : segment_targets) {
        if (!t.open) {
            t.segment = 0;
            t.dropped = false;
        }
        if (t.to_right_eye || t.to_screen) {
            t.right_eye = t.to_right_eye && !t.to_screen && !t.feeds_reader;
            // Both eyes in one buffer: the right eye's bytes, when no screen's transfer read any of them.
            const bool apart = t.to_right_eye && t.to_screen &&
                               (t.to_right_hi <= t.to_screen_lo || t.to_screen_hi <= t.to_right_lo);
            const bool hidden_apart =
                !apart && t.to_screen && !t.feeds_reader && t.to_hidden_lo < t.to_hidden_hi &&
                (t.to_hidden_hi <= t.to_screen_lo || t.to_screen_hi <= t.to_hidden_lo);
            t.feeds_reader = false;
            t.right_lo = apart ? t.to_right_lo : hidden_apart ? t.to_hidden_lo : 0;
            t.right_hi = apart ? t.to_right_hi : hidden_apart ? t.to_hidden_hi : 0;
            t.to_right_eye = t.to_screen = false;
            t.to_right_lo = t.to_right_hi = t.to_screen_lo = t.to_screen_hi = 0;
        }
        t.to_hidden_lo = t.to_hidden_hi = 0;
    }
    segment_copies.clear();
    std::erase_if(dropped_outputs, [](const DroppedOutput& o) { return o.earlier; });
    for (DroppedOutput& o : dropped_outputs) {
        o.earlier = true;
    }
    dropped_lcd.clear();
    late_reported = {};
    for (u32 i = 0; i < 2; i++) {
        screen_cost[i] = screen_cost[i] - screen_cost[i] / 8 + frame_cost[i];
    }
    frame_cost = {};
    // The ticket TicketFramesBack vblanks back, and the oldest kept, LeadFramesBack back.
    const u64 back =
        frame_tickets[(frame_ticket_at + LeadFramesBack - (TicketFramesBack - 1)) % LeadFramesBack];
    const u64 lead = frame_tickets[(frame_ticket_at + 1) % LeadFramesBack];
    const bool gpu_late = back != 0 && !InterruptPoll(back);
    for (u32 i = 0; i < 2; i++) {
        screen_skips[i] = late_skip && late_keep != static_cast<int>(i) ? screen_skips[i] + 1 : 0;
    }
    // The GPU's lag drops the busier screen whatever the guest's pace: a GPU-bound scene runs the game
    // at the emulation's speed and shows fewer pictures. MaxScreenSkips() bounds the drops in a row.
    late_skip = gpu_late;
    if (late_skip) {
        late_keep = screen_cost[0] >= screen_cost[1] ? 1 : 0;
        if (screen_skips[1 - late_keep] >= MaxScreenSkips()) {
            late_skip = false;
        }
    }
    // Drawn whole while the GPU lags: the vblank waits for the frame LeadFramesBack before, so the GPU
    // has a frame's work queued at the wake.
    SgxdTrace("late", !late_skip ? (gpu_late ? 1u : 0u) : static_cast<u32>(late_keep == 1 ? 2 : 3));
    if (gpu_late && !late_skip) {
#ifdef VITA_DIAGNOSTICS
        const auto wait_start = Clock::now();
#endif
        SgxdTrace("lead_wait");
        InterruptWait(lead);
        SgxdTrace("lead_wait_end");
#ifdef VITA_DIAGNOSTICS
        late_waits.fetch_add(1, std::memory_order_relaxed);
        late_wait_us.fetch_add(
            static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - wait_start)
                    .count()),
            std::memory_order_relaxed);
#endif
    }
#ifdef VITA_DIAGNOSTICS
    {
        const u32 lead = frames_put - frames_done.load(std::memory_order_relaxed);
        const u32 bytes = write - read.load(std::memory_order_relaxed);
        lead_frames_sum.fetch_add(lead, std::memory_order_relaxed);
        lead_samples.fetch_add(1, std::memory_order_relaxed);
        if (lead > lead_frames_max.load(std::memory_order_relaxed)) {
            lead_frames_max.store(lead, std::memory_order_relaxed);
        }
        if (bytes > lead_bytes_max.load(std::memory_order_relaxed)) {
            lead_bytes_max.store(bytes, std::memory_order_relaxed);
        }
        frames_put++;
    }
#endif
    if (late_skip) {
#ifdef VITA_DIAGNOSTICS
        late_frames.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    Put(Op::FrameEnd);
}

bool RenderThread::RightEyeOutput(PAddr addr, u32 bytes) const {
    if (!right_eye_disabler.Allowed()) {
        return false;
    }
    const u32 size = lcd_fb[0].stride * 400u;
    return std::any_of(right_eye_buffers.begin(), right_eye_buffers.end(), [&](const LcdBuffer& b) {
        return addr < b.addr + size && b.addr < addr + bytes;
    });
}

int RenderThread::LcdScreenOf(PAddr addr, u32 bytes, PAddr* buffer) const {
    for (u32 i = 0; i < 2; i++) {
        const u32 size = lcd_fb[i].stride * (i == 0 ? 400u : 320u);
        for (const LcdBuffer& b : lcd_buffers[i]) {
            if (addr < b.addr + size && b.addr < addr + bytes) {
                if (buffer) {
                    *buffer = b.addr;
                }
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

void RenderThread::NoteLcdWrite(int screen, PAddr buffer, bool dropped) {
    for (LcdBuffer& b : lcd_buffers[screen]) {
        if (b.addr == buffer) {
            b.gpu = true;
            b.stamp = dropped ? b.stamp : ++lcd_writes;
        }
    }
}

RenderThread::SegmentTarget& RenderThread::SegmentTargetFor(PAddr addr, u32 bytes) {
    for (SegmentTarget& t : segment_targets) {
        if (t.addr == addr && t.bytes == bytes) {
            return t;
        }
    }
    late_generation++;
    if (segment_targets.size() == 16) {
        segment_targets.erase(segment_targets.begin());
    }
    SegmentTarget& t = segment_targets.emplace_back(
        SegmentTarget{addr, bytes, 0, false, false, {}, 0, 0, false, false, false});
    t.screens.fill(-1);
    return t;
}

bool RenderThread::LateDraw(ListDraw& d, u32 vertices) {
    const auto& fb = pica.regs.internal.framebuffer.framebuffer;
    const PAddr addr = fb.GetColorBufferPhysicalAddress();
    const u32 bytes =
        fb.GetWidth() * fb.GetHeight() * Pica::FramebufferRegs::BytesPerColorPixel(fb.color_format);
    // A draw straight into an LCD buffer is that screen's; otherwise its segment's.
    if (late_cache.addr != addr || late_cache.bytes != bytes ||
        late_cache.generation != late_generation) {
        late_cache.lcd = 0;
        late_cache.lcd_screen = LcdScreenOf(addr, bytes, &late_cache.lcd);
        late_cache.target =
            static_cast<u32>(&SegmentTargetFor(addr, bytes) - segment_targets.data());
        late_cache.addr = addr;
        late_cache.bytes = bytes;
        late_cache.generation = late_generation;
    }
    const PAddr lcd = late_cache.lcd;
    const int lcd_screen = late_cache.lcd_screen;
    SegmentTarget& t = segment_targets[late_cache.target];
    t.open = true;
    t.drawn = ++segment_draws;
    // In a buffer of both eyes, a draw whose viewport's rows lie within the right eye's bytes (rows
    // y0..y1 are bytes from (height - y1) rows in to (height - y0)).
    const auto in_right_eye = [&] {
        if (t.right_lo >= t.right_hi) {
            return false;
        }
        const auto rect = pica.regs.internal.rasterizer.GetViewportRect();
        const u32 height = fb.GetHeight();
        const u32 row = fb.GetWidth() * Pica::FramebufferRegs::BytesPerColorPixel(fb.color_format);
        const u32 y0 = static_cast<u32>(std::clamp<s32>(rect.bottom, 0, static_cast<s32>(height)));
        const u32 y1 = static_cast<u32>(std::clamp<s32>(rect.top, 0, static_cast<s32>(height)));
        return y0 < y1 && (height - y1) * row >= t.right_lo && (height - y0) * row <= t.right_hi;
    };
    const bool right_part = lcd_screen < 0 && !t.right_eye && in_right_eye();
    const u32 slot = std::min<u32>(t.segment, SegmentSlots - 1);
    const int screen = lcd_screen >= 0              ? lcd_screen
                       : t.right_eye || right_part ? RightEyeScreen
                                                   : t.screens[slot];
    if (screen >= 0 && screen != RightEyeScreen) {
        frame_cost[screen] += vertices + LateDrawCost;
    }
    // The screens whose draws sample a colour buffer, for one no transfer ends (an effect's chain of
    // buffers goes with the screens at its end).
    const u8 draw_readers = screen >= 0 && screen != RightEyeScreen
                                ? static_cast<u8>(1u << screen)
                                : (screen < 0 ? t.readers : u8{0});
    if (draw_readers != 0) {
        for (const auto& texture : pica.regs.internal.texturing.GetTextures()) {
            if (!texture.enabled) {
                continue;
            }
            const PAddr read = texture.config.GetPhysicalAddress();
            for (SegmentTarget& s : segment_targets) {
                if (&s != &t && read >= s.addr && read < s.addr + s.bytes) {
                    s.readers |= draw_readers;
                }
            }
        }
    }
    const bool right_eye = screen == RightEyeScreen;
    // A buffer of no screen goes with the screens that sample it: dropped late unless the kept screen
    // reads it.
    const bool reader_kept = screen < 0 && late_keep >= 0 && ((t.readers >> late_keep) & 1) != 0;
    const bool dropped =
        right_eye || (late_skip && !reader_kept && (screen < 0 || screen != late_keep));
    // A kept draw that samples a copy of another buffer: that buffer feeds it, whatever screen its own
    // segment goes to (each eye's picture may be composited in the other eye's buffer).
    if (!right_eye && !segment_copies.empty()) {
        for (const auto& texture : pica.regs.internal.texturing.GetTextures()) {
            if (!texture.enabled) {
                continue;
            }
            const PAddr read = texture.config.GetPhysicalAddress();
            for (const SegmentCopy& c : segment_copies) {
                if (read < c.lo || read >= c.hi || (c.source == t.addr && c.source_bytes == t.bytes)) {
                    continue;
                }
                for (SegmentTarget& s : segment_targets) {
                    if (s.addr == c.source && s.bytes == c.source_bytes) {
#ifdef VITA_DIAGNOSTICS
                        static u32 marks = 0;
                        if (!s.feeds_reader && marks < 200 && LatelogNow()) {
                            marks++;
                            LOG_INFO(Render,
                                     "latelog: frame {}: a draw into {:#x}+{:#x} (screen {}) reads "
                                     "{:#x}, a copy {:#x}-{:#x} of {:#x}+{:#x}: that one feeds it",
                                     screenlog_frame, addr, bytes, screen, read, c.lo, c.hi,
                                     s.addr, s.bytes);
                        }
#endif
                        s.feeds_reader = true;
                    }
                }
            }
        }
    }
#ifdef VITA_DIAGNOSTICS
    static u32 hazards = 0;
    if (dropped) {
        t.dropped_frame = screenlog_frame;
    } else {
        t.kept_frame = screenlog_frame;
    }
    if (!dropped && hazards < 400 && LatelogNow()) {
        for (const auto& texture : pica.regs.internal.texturing.GetTextures()) {
            const PAddr read = texture.config.GetPhysicalAddress();
            for (const SegmentTarget& s : segment_targets) {
                if (texture.enabled && read >= s.addr && read < s.addr + s.bytes &&
                    s.dropped_frame != 0 && s.dropped_frame >= s.kept_frame && hazards++ < 400) {
                    LOG_INFO(Render,
                             "latelog: frame {}: a kept draw into {:#x} (screen {}) reads {:#x} "
                             "of {:#x}+{:#x} (screen {}), whose newest draws were dropped {} "
                             "frame(s) back (last kept {} back)",
                             screenlog_frame, addr, screen, read, s.addr, s.bytes,
                             s.screens[std::min<u32>(s.segment, SegmentSlots - 1)],
                             screenlog_frame - s.dropped_frame, screenlog_frame - s.kept_frame);
                }
            }
        }
    }
    if (screenlog_on) {
        auto it = std::find_if(screenlog_targets.begin(), screenlog_targets.end(),
                               [&](const ScreenlogTarget& s) {
                                   return s.addr == addr && s.bytes == bytes && s.screen == screen;
                               });
        if (it == screenlog_targets.end()) {
            screenlog_targets.push_back({addr, bytes, screen, 0, 0});
            it = screenlog_targets.end() - 1;
        }
        it->draws++;
        it->dropped += dropped ? 1 : 0;
    }
#endif
    if (lcd_screen >= 0) {
        NoteLcdWrite(lcd_screen, lcd, dropped);
    }
    // The rasterizer told of a dropped draw, once for each buffer and viewport a frame: a game's
    // double render (RasterizerGxm::DoubleRender) whose blend back a late frame drops ends there.
    if (dropped) {
        const auto& rs = pica.regs.internal.rasterizer;
        const std::array<u32, 3> report{
            addr, static_cast<u32>(Pica::f24::FromRaw(rs.viewport_size_x).ToFloat32() * 2),
            static_cast<u32>(Pica::f24::FromRaw(rs.viewport_size_y).ToFloat32() * 2)};
        if (report != late_reported) {
            late_reported = report;
            Put(Op::LateDropped, report);
        }
    }
    if (!dropped) {
#ifdef VITA_DIAGNOSTICS
        if (late_skip) {
            late_kept.fetch_add(1, std::memory_order_relaxed);
        }
#endif
        return false;
    }
#ifdef VITA_DIAGNOSTICS
    (right_eye ? right_eye_draws : late_draws).fetch_add(1, std::memory_order_relaxed);
#endif
    // A draw of the right eye's bytes alone leaves what the buffer's other transfers read whole.
    if (!right_part) {
        t.dropped = true;
    }
    return true;
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
    Wake();
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
    {
        std::scoped_lock lk{work};
        park_requested = false;
        work.NotifyAll();
    }
    Wake();
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
    const u32 gpu_wakes = sgxd_wake_count();
    s.gpu_wakes = gpu_wakes - gpu_wakes_mark;
    gpu_wakes_mark = gpu_wakes;
    s.timeouts = timeouts.exchange(0);
    s.presents = presents.exchange(0);
    s.presents_behind = presents_behind.exchange(0);
    s.presents_soon = presents_soon.exchange(0);
    s.late_frames = late_frames.exchange(0);
    s.late_waits = late_waits.exchange(0);
    s.late_wait_us = late_wait_us.exchange(0);
    s.lead_frames_sum = lead_frames_sum.exchange(0);
    s.lead_samples = lead_samples.exchange(0);
    s.lead_frames_max = lead_frames_max.exchange(0);
    s.lead_bytes_max = lead_bytes_max.exchange(0);
    s.late_draws = late_draws.exchange(0);
    s.late_kept = late_kept.exchange(0);
    s.merged_draws = merged_draws.exchange(0);
    s.merged_runs = merged_runs.exchange(0);
    s.right_eye_draws = right_eye_draws.exchange(0);
    s.right_eye_transfers = right_eye_transfers.exchange(0);
    s.late_partial = late_partial.exchange(0);
    s.parse_busy_us = parse_busy_us.exchange(0);
    s.parsed_waits = parsed_waits.exchange(0);
    s.parsed_wait_us = parsed_wait_us.exchange(0);
    s.lists = lists.exchange(0);
    s.list_bytes = list_bytes.exchange(0);
    s.list_parsed_bytes = list_parsed_bytes.exchange(0);
    s.list_ranges = list_ranges.exchange(0);
    s.list_parsed_max = list_parsed_max.exchange(0);
    s.list_draws = list_draws.exchange(0);
    s.scan_us = scan_us.exchange(0);
    s.index_scans = index_scans.exchange(0);
    s.index_scan_indices = index_scan_indices.exchange(0);
    s.index_scan_us = index_scan_us.exchange(0);
    s.index_memo_hits = index_memo_hits.exchange(0);
    s.publishes = publishes.exchange(0);
    s.render_wakes = render_wakes.exchange(0);
    s.parse_wakes = parse_wakes.exchange(0);
    s.wake_us = wake_us.exchange(0);
    s.gpu_samples = gpu_samples.exchange(0);
    s.gpu_busy = gpu_busy.exchange(0);
    s.gpu_held = gpu_held.exchange(0);
    for (u32 k = 0; k < GpuAtCells; k++) {
        s.gpu_at[k] = gpu_at[k].exchange(0);
    }
    for (u32 k = 0; k < GpuLabels; k++) {
        s.gpu_3d_by[k] = gpu_3d_by[k].exchange(0);
        s.gpu_ta_by[k] = gpu_ta_by[k].exchange(0);
        s.jobs_by[k] = __atomic_exchange_n(&sgxd_mkq_label_sent[k], 0u, __ATOMIC_RELAXED);
    }
    for (u32 k = 0; k < s.present_gaps.size(); k++) {
        s.present_gaps[k] = present_gaps[k].exchange(0);
    }
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

bool RenderThread::Sleep(std::chrono::microseconds timeout) {
    SceUInt us = static_cast<SceUInt>(timeout.count());
    unsigned int bits = 0;
    const int r = sceKernelWaitEventFlag(wake_flag, WakeBit,
                                         SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &us);
    ASSERT_MSG(r == 0 || r == SCE_KERNEL_ERROR_WAIT_TIMEOUT, "render thread: wait {:#010x}; {}",
               static_cast<u32>(r), Common::DescribeWaiters(wake_flag));
    return r == 0;
}

void RenderThread::Wake() {
#ifdef VITA_DIAGNOSTICS
    const auto start = Clock::now();
#endif
    sceKernelSetEventFlag(wake_flag, WakeBit);
#ifdef VITA_DIAGNOSTICS
    render_wakes.fetch_add(1, std::memory_order_relaxed);
    wake_us.fetch_add(Since(start), std::memory_order_relaxed);
#endif
}

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
    // Counted as it goes, or a loop that never runs dry would show no time in perflog.
    auto start = Clock::now();
    u32 since_busy = 0;
#endif
    // A pending present goes once its interval is up, between records: the clock is read every 64
    // records only (a syscall), the window's present every 8.
    u32 since_check = 0;
    u32 since_flip = 0;
    while (r != published.load(std::memory_order_acquire)) {
        Head head;
        const u8* const p = ring.get() + r % RingSize;
        std::memcpy(&head, p, sizeof(head));
        if (head.op != Op::Wrap) {
            running_op.store(static_cast<u32>(head.op), std::memory_order_relaxed);
            RunRecord(head, p + sizeof(Head));
            running_op.store(NoRecord, std::memory_order_relaxed);
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
        if ((++since_flip & 7) == 0) {
            renderer.FlipDone();
        }
        if (present_pending && (++since_check & 63) == 0) {
            ShowDuePresent();
        }
#ifdef VITA_DIAGNOSTICS
        if ((++since_busy & 255) == 0) {
            const auto now = Clock::now();
            busy_us.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                   now - start)
                                                   .count()),
                              std::memory_order_relaxed);
            start = now;
        }
#endif
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

void RenderThread::ShowPresent(const std::array<Pica::FramebufferConfig, 2>& fb,
                               const std::array<Pica::ColorFill, 2>& fill,
                               Common::ProcessClock::time_point now) {
#ifdef VITA_DIAGNOSTICS
    const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_present);
    present_gaps[gap.count() <= 20   ? 0
                 : gap.count() <= 37 ? 1
                 : gap.count() <= 54 ? 2
                                     : 3]
        .fetch_add(1, std::memory_order_relaxed);
#endif
    last_present = now;
#ifdef VITA_DIAGNOSTICS
    const PAddr top = fb[0].second_fb_active ? fb[0].address_left2 : fb[0].address_left1;
    SgxdTrace("present", top != trace_top ? 1u : 0u);
    trace_top = top;
#endif
    renderer.PresentFrame(fb, fill);
    shown_frames.fetch_add(1, std::memory_order_relaxed);
    SgxdTrace("presented");
    rasterizer.GpuIdle();
#ifdef VITA_DIAGNOSTICS
    presents.fetch_add(1, std::memory_order_relaxed);
#endif
}

void RenderThread::ShowDuePresent() {
    if (!present_pending) {
        return;
    }
    const auto now = Clock::now();
    if (now - last_present < PresentInterval || renderer.PresentBusy()) {
        return;
    }
    present_pending = false;
    ShowPresent(pending_fb, pending_fill, now);
}

void RenderThread::RunRecord(const Head& head, const u8* p) {
    const auto take = [p]<typename T>(T& out) { std::memcpy(&out, p, sizeof(T)); };
    switch (head.op) {
    case Op::Wrap:
        break;
    case Op::ListDraws:
    case Op::RegWrite:
        // The parse thread's (ParseRecord).
        break;
    case Op::CmdList: {
        CmdListRecord c;
        take(c);
        RunParsed(c.seq);
        break;
    }
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
        if ((h.f[0] | h.f[1] | h.f[2]) != 0) {
            setup.f_version++;
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
        rasterizer.FogLutChanged();
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
        DrawIndexRange indices;
        take(indices);
        if (indices.scanned != 0) {
            rasterizer.ScannedIndices(indices.lo, indices.hi);
        }
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
    case Op::LateDropped: {
        std::array<u32, 3> dropped;
        take(dropped);
        rasterizer.LateDropped(dropped[0], dropped[1], dropped[2]);
        break;
    }
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
        // The late skip's dropped transfers are not recorded (RenderThread::LateTransfer).
        const bool display = head.op == Op::DisplayTransfer;
        if (display) {
            rasterizer.AccelerateDisplayTransfer(config);
        } else {
            rasterizer.AccelerateTextureCopy(config);
        }
        break;
    }
    case Op::Dma: {
        DmaRecord d;
        take(d);
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
        // Two newer presents queued: the thread is more than the vblank's one present behind, and the
        // newest shows a newer picture. Not with none shown for PresentStarved.
        const u32 behind = presents_queued.fetch_sub(1, std::memory_order_relaxed) - 1;
        if (behind >= 2 && Clock::now() - last_present < PresentStarved) {
#ifdef VITA_DIAGNOSTICS
            presents_behind.fetch_add(1, std::memory_order_relaxed);
#endif
            SgxdTrace("present_behind");
            break;
        }
        const auto now = Clock::now();
        if (now - last_present < PresentInterval || renderer.PresentBusy()) {
            // Too soon after the last, or the last still on the GPU: shown later unless a newer one comes.
#ifdef VITA_DIAGNOSTICS
            if (present_pending) {
                presents_soon.fetch_add(1, std::memory_order_relaxed);
            }
#endif
            present_pending = true;
            pending_fb = r.fb;
            pending_fill = r.fill;
            SgxdTrace("present_pending");
            break;
        }
#ifdef VITA_DIAGNOSTICS
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_present);
        present_gaps[gap.count() <= 20   ? 0
                     : gap.count() <= 37 ? 1
                     : gap.count() <= 54 ? 2
                                         : 3]
            .fetch_add(1, std::memory_order_relaxed);
#endif
        last_present = now;
        GxmTrace("present");
        renderer.PresentFrame(r.fb, r.fill);
        GxmTrace("presented");
        rasterizer.GpuIdle();
#ifdef VITA_DIAGNOSTICS
        presents.fetch_add(1, std::memory_order_relaxed);
#endif
        present_pending = false;
        ShowPresent(r.fb, r.fill, now);
        break;
    }
    case Op::FrameEnd:
        rasterizer.EndFrame();
#ifdef VITA_DIAGNOSTICS
        frames_done.fetch_add(1, std::memory_order_relaxed);
#endif
        break;
    case Op::GuestWrote: {
        std::array<u32, 2> range;
        take(range);
        rasterizer.GuestWrote(range[0], range[1]);
        break;
    }
    case Op::LateDropped: {
        std::array<u32, 3> dropped;
        take(dropped);
        rasterizer.LateDropped(dropped[0], dropped[1], dropped[2]);
        break;
    }
    case Op::Check:
        RunCheck(p);
        break;
    case Op::PageRemap: {
        std::array<u32, SGXD_REMAP_MAX> pages, pte_va, pte;
        bool direct = false;
        for (u32 k = 0; k < head.extra; k++) {
            PageHolds::Remap r;
            std::memcpy(&r, p + k * sizeof(r), sizeof(r));
            pages[k] = r.page;
            pte_va[k] = r.pte_va;
            pte[k] = r.pte;
            direct |= r.direct != 0;
        }
        rasterizer.RemapPages(pages.data(), pte_va.data(), pte.data(), head.extra, direct);
        break;
    }
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

void RenderThread::SetMaxScreenSkips(u32 frames) {
    max_screen_skips.store(frames, std::memory_order_relaxed);
}

u32 RenderThread::MaxScreenSkips() {
    return max_screen_skips.load(std::memory_order_relaxed);
}

s32 RenderThread::LiveThreadId() {
    const RenderThread* const t = live_render_thread.load(std::memory_order_acquire);
    return t != nullptr ? t->render_tid.load(std::memory_order_relaxed) : -1;
}

void RenderThread::DescribeLive(char* out, std::size_t size) {
    const RenderThread* const t = live_render_thread.load(std::memory_order_acquire);
    if (t == nullptr) {
        std::snprintf(out, size, "no render thread");
        return;
    }
    const u64 front = t->pending_front_fence.load(std::memory_order_relaxed);
    // The thread's CPU time and core (it runs or it is starved) and sgxd's interrupts (a storm).
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const s32 tid = t->render_tid.load(std::memory_order_relaxed);
    const bool have_info = tid >= 0 && sceKernelGetThreadInfo(tid, &info) >= 0;
    const volatile SgxdEvents* const ev = sgxd_ptla_page();
    const int lead = std::snprintf(
        out, size, "run time %llu us on core %d (last %d); interrupts %u (SW_EVENT %u, other %#x); ",
        have_info ? static_cast<unsigned long long>(info.runClocks) : 0ull,
        have_info ? static_cast<int>(info.currentCpuId) : -1,
        have_info ? static_cast<int>(info.lastExecutedCpuId) : -1, ev ? ev->irqs : 0u,
        ev ? ev->sw_events : 0u, ev ? ev->unexpected : 0u);
    if (lead > 0 && static_cast<std::size_t>(lead) < size) {
        out += lead;
        size -= static_cast<std::size_t>(lead);
    }
    std::snprintf(
        out, size,
        "loop passes %u; running record op %d; tickets issued %u, reached %u; the emulation "
        "waits for %u (%s); "
        "pending %u, the oldest ticket %u (fence: lists %u, PTLA ops %u); lists sent %u, done %u, "
        "faulted %u; PTLA ops sent %u, done %u; a 3D pending %s; sleeping %s; hold depth %u, "
        "holder %08x, park requested %s, parked %s; ring written %u, published %u, read %u",
        t->loop_passes.load(std::memory_order_relaxed),
        static_cast<int>(t->running_op.load(std::memory_order_relaxed)), t->tickets,
        t->reached.load(std::memory_order_relaxed),
        t->waited_ticket.load(std::memory_order_relaxed),
        t->ticket_wait.load(std::memory_order_relaxed) ? "waiting" : "not waiting",
        t->pending_size.load(std::memory_order_relaxed),
        t->pending_front_ticket.load(std::memory_order_relaxed), static_cast<u32>(front >> 32),
        static_cast<u32>(front), sgxd_chain_fence(), sgxd_chain_done(), sgxd_chain_faulted(),
        sgxd_ptla_fence(), sgxd_ptla_done(), sgxd_session_pending() != nullptr ? "yes" : "no",
        t->sleeping.load(std::memory_order_relaxed) ? "yes" : "no", t->hold_depth,
        static_cast<u32>(t->holder.load(std::memory_order_relaxed)),
        t->park_requested ? "yes" : "no", t->parked.load(std::memory_order_relaxed) ? "yes" : "no",
        t->write, t->published.load(std::memory_order_relaxed),
        t->read.load(std::memory_order_relaxed));
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
    render_tid.store(sceKernelGetThreadId(), std::memory_order_relaxed);
    u32 progress = 0;
    auto progress_at = Clock::now();
    for (;;) {
        RunAvailable();
        PollFences();
        loop_passes.fetch_add(1, std::memory_order_relaxed);
        pending_size.store(static_cast<u32>(pending.size()), std::memory_order_relaxed);
        pending_front_ticket.store(pending.empty() ? 0 : pending.front().ticket,
                                   std::memory_order_relaxed);
        pending_front_fence.store(pending.empty() ? 0 : pending.front().fence,
                                  std::memory_order_relaxed);
        renderer.FlipDone();
        ShowDuePresent();
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
        // One clock read for the checks below (Clock::now() is a syscall).
        auto now = Clock::now();
        if (!pending.empty()) {
            const u32 now_progress = sgxd_chain_done() + sgxd_ptla_done();
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
                now = Clock::now();
                progress_at = now;
            }
        }
        // The window between presents: a poll may run the pause menu, so it goes outside the work lock.
        if (now - last_window_poll >= WindowPollInterval) {
            renderer.PollWindow();
            now = Clock::now();
            last_window_poll = now;
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
            // With tickets pending, sgxd wakes the thread once the oldest one's GPU work is done
            // (or the lists before a held-back 3D are, for PollFences to send it), or the
            // window's last present's when that comes first, for its flip (FlipDone). Records, a
            // hold and the end wake it through Wake; the window's poll bounds the sleep.
            u32 flip_fence;
            const bool flip =
                renderer.FlipPending(&flip_fence) &&
                (pending.empty() ||
                 static_cast<s32>(static_cast<u32>(pending.front().fence >> 32) - flip_fence) > 0);
            const bool waiting = flip || !pending.empty();
            const bool armed = flip ? rasterizer.FlipArm(flip_fence)
                                    : waiting && rasterizer.InterruptArm(pending.front().fence);
            if (!waiting || armed) {
#ifdef VITA_DIAGNOSTICS
                const auto start = now;
#endif
                work.unlock();
                auto sleep =
                    std::chrono::duration_cast<std::chrono::microseconds>(WindowPollInterval);
                if (present_pending) {
                    // Its interval, or the last present's GPU work, polled every 2 ms.
                    sleep = std::clamp(std::chrono::duration_cast<std::chrono::microseconds>(
                                           last_present + PresentInterval - now),
                                       std::chrono::microseconds(2000), sleep);
                }
                // The window's present drawn and held for its paced flip (FlipDone).
                if (const s64 due_in = renderer.FlipDueIn(); due_in > 0) {
                    sleep = std::min(sleep, std::chrono::microseconds(std::max<s64>(due_in, 500)));
                }
                [[maybe_unused]] const bool woken = Sleep(sleep);
                work.lock();
                if (waiting) {
                    rasterizer.InterruptDisarm();
#ifdef VITA_DIAGNOSTICS
                    if (!woken) {
                        timeouts.fetch_add(1, std::memory_order_relaxed);
                    }
#endif
                }
#ifdef VITA_DIAGNOSTICS
                idle_us.fetch_add(Since(start), std::memory_order_relaxed);
#endif
            }
        }
        sleeping.store(false);
    }
}

} // namespace SgxdRenderer
