// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/pipeline_stats.h"
#include "video_core/renderer_gxm/gxm_flags.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <new>
#include "video_core/gpu_thread.h"
#include "video_core/pica/pica_core.h"
#include <cstring>
#include "video_core/guest_watch.h"
#include "common/emu_prof.h"
#include "common/pipeline_stats.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "common/vita_tuning.h"
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#endif
#include "common/vita_data_dir.h"
#include "common/logging/log.h"
#include "video_core/threaded_rasterizer.h"

namespace VideoCore {

void ThreadedRasterizer::ApplySetup(Pica::ShaderSetup& setup, const SetupSync& sync) {
    if (sync.has_uniforms) {
        // Register by register, so the fingerprint the geometry-shader draw memo keys on
        // follows what changed rather than being recomputed over all 96.
        for (u32 i = sync.f_lo; i < sync.f_hi; i++) {
            if (std::memcmp(&setup.uniforms.f[i], &sync.uniforms.f[i], sizeof(setup.uniforms.f[i])) !=
                0) {
                setup.SetFloatUniformTracked(i, sync.uniforms.f[i]);
            }
        }
        setup.uniforms.b = sync.uniforms.b;
        setup.uniforms.i = sync.uniforms.i;
        setup.uniforms_dirty = true;
        // The mirror's own window: what a renderer keeping a converted copy of the bank has
        // to convert again.
        setup.sync_f_lo = std::min(setup.sync_f_lo, sync.f_lo);
        setup.sync_f_hi = std::max(setup.sync_f_hi, sync.f_hi);
    }
    if (sync.has_code) {
        const auto& code = *sync.code;
        if (code.has_program) {
            setup.LoadShippedProgram(code.program.data(), static_cast<u32>(code.program.size()),
                                     code.program_hash);
        }
        if (code.has_swizzle) {
            setup.LoadShippedSwizzle(code.swizzle.data(), static_cast<u32>(code.swizzle.size()),
                                     code.swizzle_hash);
        }
    }
}

/// Applies a shipment's state to the render thread's mirror.
void ThreadedRasterizer::ApplyShipment(Pica::PicaCore& mirror, const Shipment& shipment,
                                       bool count_stats) {
    // The blocks a software draw's fragment side reads (RasterizerGxm::RepeatKey), as one
    // register index range each.
    static constexpr std::array<std::pair<u32, u32>, 4> fragment_blocks{{
        {PICA_REG_INDEX(rasterizer), sizeof(Pica::RasterizerRegs) / 4},
        {PICA_REG_INDEX(texturing), sizeof(Pica::TexturingRegs) / 4},
        {PICA_REG_INDEX(framebuffer), sizeof(Pica::FramebufferRegs) / 4},
        {PICA_REG_INDEX(lighting), sizeof(Pica::LightingRegs) / 4},
    }};
    // Registers inside those blocks that a software draw's fragment side never reads, or whose
    // effect has its own dirty flag the repeat paths check: the vertex shader's output map and
    // clock (0x4F-0x56, 0x6E-0x7F) and the fog, procedural-texture and lighting LUT data ports
    // (lut_dirty / table_dirty). MH4U rewrites 0x6F around its particle draws. `allfragregs`
    // counts them again.
    static const std::array<bool, 0x300> fragment_ignored = [] {
        std::array<bool, 0x300> t{};
        if (GxmRenderer::GxmFlag("allfragregs")) {
            return t;
        }
        const auto mark = [&t](u32 first, u32 count) {
            for (u32 i = first; i < first + count && i < t.size(); i++) {
                t[i] = true;
            }
        };
        mark(PICA_REG_INDEX(rasterizer.vs_output_total), 8);
        mark(PICA_REG_INDEX(rasterizer.depthmap_enable) + 1, 0x12);
        mark(PICA_REG_INDEX(texturing.fog_lut_offset), 1);
        mark(PICA_REG_INDEX(texturing.fog_lut_data), 8);
        mark(PICA_REG_INDEX(texturing.proctex_lut_config), 1);
        mark(PICA_REG_INDEX(texturing.proctex_lut_data), 8);
        mark(PICA_REG_INDEX(lighting.lut_config), 1);
        mark(PICA_REG_INDEX(lighting.lut_data), 8);
        return t;
    }();
    // The registers a hardware draw's vertex layout and program key read
    // (RasterizerGxm::AccelerateShippedDraw, VertexShaderFor): the attribute formats and each
    // loader's two configuration words - not the buffer addresses, which move every draw -
    // the vertex shader's configuration words (bools, integers, input count, entry point,
    // input map, output mask; not its upload ports), the GS switch, the VS output map, and
    // the lighting disable bit.
    static const std::array<bool, 0x300> vertex_reads = [] {
        std::array<bool, 0x300> t{};
        const u32 va = PICA_REG_INDEX(pipeline.vertex_attributes);
        t[va + 1] = t[va + 2] = true;
        for (u32 loader = 0; loader < 12; loader++) {
            t[va + 3 + loader * 3 + 1] = t[va + 3 + loader * 3 + 2] = true;
        }
        t[PICA_REG_INDEX(pipeline.num_vertices) + 1] = true; // use_gs
        for (u32 i = PICA_REG_INDEX(vs); i < PICA_REG_INDEX(vs) + 0x10; i++) {
            t[i] = true;
        }
        for (u32 i = 0; i < 8; i++) {
            t[PICA_REG_INDEX(rasterizer.vs_output_total) + i] = true;
        }
        t[PICA_REG_INDEX(lighting.disable)] = true;
        return t;
    }();
    // Texture-binding registers (RasterizerGxm's texture-only fast path): each unit's
    // dimensions, addresses and format, and its configuration word when only filter bits move
    // (type and wrap modes unchanged). Everything else in the fragment blocks is non-binding.
    static const std::array<bool, 0x300> binding = [] {
        std::array<bool, 0x300> t{};
        for (const u32 i : {0x82u, 0x92u, 0x9Au, 0x85u, 0x86u, 0x87u, 0x88u, 0x89u, 0x8Au,
                            0x95u, 0x9Du, 0x8Eu, 0x96u, 0x9Eu}) {
            t[i] = true;
        }
        return t;
    }();
    static_assert(PICA_REG_INDEX(texturing.texture0) == 0x81 &&
                  PICA_REG_INDEX(texturing.texture1) == 0x91 &&
                  PICA_REG_INDEX(texturing.texture2) == 0x99);
    bool fragment_changed = false;
    bool nonbinding_changed = false;
    bool vertex_changed = false;
    for (const auto& [index, value] : shipment.reg_delta) {
        u32& reg = mirror.regs.internal.reg_array[index];
        if (reg != value) {
            const bool filter_only = (index == 0x83 || index == 0x93 || index == 0x9B) &&
                                     ((reg ^ value) & 0x70007700u) == 0;
            const bool is_binding = (index < binding.size() && binding[index]) || filter_only;
            vertex_changed |= index < vertex_reads.size() && vertex_reads[index];
            bool in_block = false;
            for (const auto& [first, count] : fragment_blocks) {
                in_block |= index - first < count;
            }
            in_block &= !fragment_ignored[index];
            fragment_changed |= in_block;
            nonbinding_changed |= in_block && !is_binding;
            if (count_stats && in_block &&
                index < Common::PipelineStats::frag_reg_changes.size()) {
                Common::PipelineStats::frag_reg_changes[index]++;
            }
        }
        reg = value;
    }
    mirror.fragment_regs_versioned = true;
    if (fragment_changed) {
        mirror.fragment_regs_version++;
    }
    if (nonbinding_changed) {
        mirror.fragment_nonbinding_version++;
    }

    vertex_changed |= shipment.vs.has_code ||
                      (shipment.vs.has_uniforms &&
                       (mirror.vs_setup.uniforms.b != shipment.vs.uniforms.b ||
                        mirror.vs_setup.uniforms.i != shipment.vs.uniforms.i));
    if (vertex_changed) {
        mirror.vertex_regs_version++;
    }
    ApplySetup(mirror.vs_setup, shipment.vs);
    ApplySetup(mirror.gs_setup, shipment.gs);
    if (shipment.pa_reconfigure_events != 0) {
        mirror.GetPrimitiveAssembler().Reconfigure(
            static_cast<Pica::PipelineRegs::TriangleTopology>(shipment.pa_topology));
    }
    if (shipment.pa_reset_events != 0) {
        mirror.GetPrimitiveAssembler().Reset();
    }
    if (shipment.has_default_attrs) {
        mirror.input_default_attributes = shipment.default_attrs;
    }
    mirror.dirty_regs.Or(shipment.dirty);
    if (shipment.lighting_dirty != 0) {
        static_assert(sizeof(mirror.lighting.luts[0]) == sizeof((*shipment.lighting_luts)[0]));
        u32 bits = shipment.lighting_dirty;
        while (bits != 0) {
            const u32 lut = static_cast<u32>(__builtin_ctz(bits));
            bits &= bits - 1;
            std::memcpy(&mirror.lighting.luts[lut], &(*shipment.lighting_luts)[lut],
                        sizeof(mirror.lighting.luts[lut]));
        }
        mirror.lighting.lut_dirty |= shipment.lighting_dirty;
    }
    if (shipment.fog) {
        mirror.fog = *shipment.fog;
        mirror.fog.lut_dirty = true;
    }
    if (shipment.proctex) {
        const u8 dirty = shipment.proctex->table_dirty | mirror.proctex.table_dirty;
        mirror.proctex = *shipment.proctex;
        mirror.proctex.table_dirty = dirty;
    }
}

ThreadedRasterizer::ThreadedRasterizer(GpuThread& gpu_thread_, Pica::PicaCore& emu_pica_,
                                       Pica::PicaCore& mirror_pica_)
    : gpu_thread{gpu_thread_}, emu_pica{emu_pica_}, mirror_pica{mirror_pica_} {}

void ThreadedRasterizer::SetRealRasterizer(RasterizerInterface* real_) {
    real = real_;
    // What the mirror holds is no longer known: ship the next code whole.
    shipped_vs_code = {};
    shipped_gs_code = {};
    // A recreated rasterizer brings a fresh ring (new mapping, cursor at zero); rebind and reset
    // the batch bookkeeping. This runs with the render thread quiescent.
    ring = real ? real->GetVertexRing() : nullptr;
    ranges.clear();
    batch_start_cursor = ring ? ring->cursor : 0;
    batch_start_offset = ring ? ring->offset : 0;
}

void ThreadedRasterizer::RingDrawConsumed(u64 ring_end) {
    // Render thread, after the draw that read this allocation was submitted (or dropped): the
    // floor moves to the next outstanding hardware draw, or away entirely.
    std::scoped_lock lock{ring->mutex};
    while (!ring_inflight.empty() && ring_inflight.front().second <= ring_end) {
        ring_inflight.pop_front();
    }
    ring->consume_floor.store(ring_inflight.empty() ? ~0ull : ring_inflight.front().first,
                              std::memory_order_release);
}

void ThreadedRasterizer::RingWaitForSpace(u64 bytes) {
    const u64 wait_t0 = Common::PipelineStats::NowUs();
    SCOPE_EXIT({
        Common::PipelineStats::gxw_ring_us.fetch_add(Common::PipelineStats::NowUs() - wait_t0,
                                                     std::memory_order_relaxed);
    });
    std::unique_lock lock{ring->mutex};
    while (ring->TryAllocWouldFail(static_cast<u32>(bytes))) {
        // The render thread only learns that space ran out from us: hand it a blocking retire,
        // but never more than one at a time — a queue full of blocking waits is a starved render
        // thread. Enqueue is safe here (it does not take ring->mutex), and in inline mode it
        // simply runs the retire on this thread with the context current.
        if (!retire_kick_pending.exchange(true, std::memory_order_acq_rel)) {
            gpu_thread.Enqueue({.run = [this] {
                real->RingRetireBlocking();
                retire_kick_pending.store(false, std::memory_order_release);
            }});
        }
        ring->retired_cv.wait_for(lock, std::chrono::milliseconds(2));
    }
}

std::vector<Pica::OutputVertex> ThreadedRasterizer::TakeBuffer() {
    std::scoped_lock lock{pool_mutex};
    if (buffer_pool.empty()) {
        return {};
    }
    auto buffer = std::move(buffer_pool.back());
    buffer_pool.pop_back();
    return buffer;
}

void ThreadedRasterizer::ReturnBuffer(std::vector<Pica::OutputVertex>&& buffer) {
    buffer.clear();
    std::scoped_lock lock{pool_mutex};
    if (buffer_pool.size() < 64) {
        buffer_pool.push_back(std::move(buffer));
    }
}

namespace {
/// The GL reads the ring in OutputVertex layout, so pushing a vertex is one 96-byte copy plus
/// the per-triangle quaternion sign fix the HardwareVertex conversion used to do: opposite
/// quaternions (negative dot against the provoking vertex) are negated so interpolation takes
/// the short arc. f24 stores a plain float, so the dot works on the raw fields.
float QuatDot(const Common::Vec4<Pica::f24>& qa, const Common::Vec4<Pica::f24>& qb) {
    return qa.x.ToFloat32() * qb.x.ToFloat32() + qa.y.ToFloat32() * qb.y.ToFloat32() +
           qa.z.ToFloat32() * qb.z.ToFloat32() + qa.w.ToFloat32() * qb.w.ToFloat32();
}

void NegateQuat(Pica::OutputVertex& v) {
    v.quat.x = Pica::f24::FromFloat32(-v.quat.x.ToFloat32());
    v.quat.y = Pica::f24::FromFloat32(-v.quat.y.ToFloat32());
    v.quat.z = Pica::f24::FromFloat32(-v.quat.z.ToFloat32());
    v.quat.w = Pica::f24::FromFloat32(-v.quat.w.ToFloat32());
}
} // Anonymous namespace

void ThreadedRasterizer::AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                                     const Pica::OutputVertex& v2) {
    if (ring) {
        // Zero-copy: the triangle goes straight into the mapped GL vertex buffer in guest
        // layout; no other copy of it will ever exist.
        constexpr u32 tri_bytes = 3 * sizeof(Pica::OutputVertex);
        // Under the ring's mutex: the render thread allocates from the same ring for the
        // mirror's software triangles.
        u32 off;
        {
            std::scoped_lock lock{ring->mutex};
            off = ring->TryAlloc(tri_bytes);
        }
        if (off == VertexRing::FULL) {
            RingWaitForSpace(tri_bytes);
            std::scoped_lock lock{ring->mutex};
            off = ring->TryAlloc(tri_bytes);
        }
        auto* const dst = reinterpret_cast<Pica::OutputVertex*>(ring->base + off);
        std::memcpy(dst + 0, &v0, sizeof(Pica::OutputVertex));
        std::memcpy(dst + 1, &v1, sizeof(Pica::OutputVertex));
        std::memcpy(dst + 2, &v2, sizeof(Pica::OutputVertex));
        if (QuatDot(v0.quat, v1.quat) < 0.0f) {
            NegateQuat(dst[1]);
        }
        if (QuatDot(v0.quat, v2.quat) < 0.0f) {
            NegateQuat(dst[2]);
        }

        const u32 first = off / sizeof(Pica::OutputVertex);
        if (ranges.empty() || ranges.back().first + ranges.back().count != first) {
            ranges.push_back({first, 0, 0});
        }
        ranges.back().count += 3;
        ranges.back().end_pos = ring->cursor;
        return;
    }

    batch.push_back(v0);
    batch.push_back(v1);
    batch.push_back(v2);
}

void ThreadedRasterizer::CollectState(Shipment& shipment) {
    namespace EP = Common::EmuProf;
    static const bool step_prof = EP::enabled && !GxmRenderer::GxmFlag("noregprof");
    u64 prof_t = step_prof ? Common::PipelineStats::NowUs() : 0;
    const auto prof = [&](const char* key) {
        if (step_prof) {
            const u64 now = Common::PipelineStats::NowUs();
            EP::Add(key, now - prof_t);
            prof_t = now;
        }
    };
    // Ship only the registers that changed, discovered through the dirty bits; the LUTs travel
    // only when PicaCore flagged them since the last batch.
    shipment.dirty = emu_pica.dirty_regs;
    for (u32 word = 0; word < 12; word++) {
        u64 bits = shipment.dirty.qwords[word];
        while (bits != 0) {
            const u32 bit = static_cast<u32>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const u32 index = word * 64 + bit;
            if (index < Pica::RegsInternal::NUM_REGS) {
                shipment.reg_delta.emplace_back(static_cast<u16>(index),
                                                emu_pica.regs.internal.reg_array[index]);
            }
        }
    }
    emu_pica.dirty_regs.Clear();
    prof(" .collect: regs");

    if (emu_pica.lighting.lut_dirty != 0) {
        if (!shipment.lighting_luts) {
            shipment.lighting_luts = std::make_unique<std::array<std::array<u32, 256>, 24>>();
        }
        u32 bits = emu_pica.lighting.lut_dirty & Pica::PicaCore::Lighting::LutAllDirty;
        shipment.lighting_dirty = bits;
        while (bits != 0) {
            const u32 lut = static_cast<u32>(__builtin_ctz(bits));
            bits &= bits - 1;
            std::memcpy(&(*shipment.lighting_luts)[lut], &emu_pica.lighting.luts[lut],
                        sizeof(emu_pica.lighting.luts[lut]));
        }
        emu_pica.lighting.lut_dirty = 0;
    }
    if (emu_pica.fog.lut_dirty) {
        shipment.fog = std::make_unique<Pica::PicaCore::Fog>(emu_pica.fog);
        emu_pica.fog.lut_dirty = false;
    }
    if (emu_pica.proctex.table_dirty != 0) {
        shipment.proctex = std::make_unique<Pica::PicaCore::ProcTex>(emu_pica.proctex);
        emu_pica.proctex.table_dirty = 0;
    }

    prof(" .collect: luts");
    const auto collect_setup = [&](Pica::ShaderSetup& setup, SetupSync& sync,
                                   ShippedCode& shipped) {
        if (setup.uniforms_sync_dirty) {
            sync.has_uniforms = true;
            // Only the float window written since the last collect travels; bools and ints are
            // 32 bytes and go whole. The full-struct copy here was a fixed 1.5 KB per shipment.
            sync.f_lo = std::min<u32>(setup.sync_f_lo, 96);
            sync.f_hi = std::min<u32>(setup.sync_f_hi, 96);
            if (sync.f_lo < sync.f_hi) {
                std::memcpy(&sync.uniforms.f[sync.f_lo], &setup.uniforms.f[sync.f_lo],
                            (sync.f_hi - sync.f_lo) * sizeof(setup.uniforms.f[0]));
            }
            sync.uniforms.b = setup.uniforms.b;
            sync.uniforms.i = setup.uniforms.i;
            setup.uniforms_sync_dirty = false;
            setup.sync_f_lo = 96;
            setup.sync_f_hi = 0;
        }
        prof(" .collect: uniforms");
        if (setup.code_sync_dirty) {
            // Hashed here, once per change, rather than by the busier render thread. Only a
            // half that differs from what the mirror holds is copied and shipped: the game
            // switches between a few programs, and the swizzle data rarely changes with them.
            const u64 program_hash = setup.GetProgramCodeHash();
            const u64 swizzle_hash = setup.GetSwizzleDataHash();
            const u32 program_size = setup.GetBiggestProgramSize();
            const u32 swizzle_size = setup.GetBiggestSwizzleSize();
            const bool ship_program = !shipped.valid || shipped.program_hash != program_hash ||
                                      shipped.program_size != program_size;
            const bool ship_swizzle = !shipped.valid || shipped.swizzle_hash != swizzle_hash ||
                                      shipped.swizzle_size != swizzle_size;
            if (ship_program || ship_swizzle) {
                if (!sync.code) {
                    sync.code = std::make_unique<CodeSync>();
                }
                sync.has_code = true;
                sync.code->has_program = ship_program;
                sync.code->has_swizzle = ship_swizzle;
                if (ship_program) {
                    const auto& program = setup.GetProgramCode();
                    sync.code->program.assign(program.begin(), program.begin() + program_size);
                    sync.code->program_hash = program_hash;
                }
                if (ship_swizzle) {
                    const auto& swizzle = setup.GetSwizzleData();
                    sync.code->swizzle.assign(swizzle.begin(), swizzle.begin() + swizzle_size);
                    sync.code->swizzle_hash = swizzle_hash;
                }
                shipped = {true, program_hash, program_size, swizzle_hash, swizzle_size};
            }
            setup.code_sync_dirty = false;
            prof(" .collect: code");
            if (sync.has_code && sync.code->has_program) {
                Common::PipelineStats::gxc_code_bytes.fetch_add(
                    static_cast<u32>(sync.code->program.size() * 4), std::memory_order_relaxed);
            }
            if (EP::enabled && sync.has_code && sync.code->has_program) {
                EP::Add(" .collect: code KiB", (sync.code->program.size() * 4 + 1023) / 1024 * 1000);
            }
        }
    };
    collect_setup(emu_pica.vs_setup, shipment.vs, shipped_vs_code);
    collect_setup(emu_pica.gs_setup, shipment.gs, shipped_gs_code);
    shipment.pa_reconfigure_events = emu_pica.pa_reconfigure_events;
    shipment.pa_reset_events = emu_pica.pa_reset_events;
    shipment.pa_topology =
        static_cast<u32>(emu_pica.regs.internal.pipeline.triangle_topology.Value());
    emu_pica.pa_reconfigure_events = 0;
    emu_pica.pa_reset_events = 0;
    if (emu_pica.default_attributes_sync_dirty) {
        shipment.has_default_attrs = true;
        shipment.default_attrs = emu_pica.input_default_attributes;
        emu_pica.default_attributes_sync_dirty = false;
    }
}

u8* ThreadedRasterizer::SlotAlloc(ArenaSlot& slot, u32 bytes) {
    constexpr u32 CHUNK_SIZE = 512 * 1024;
    const u32 need = std::max(bytes, 1u);
    if (slot.active_chunk >= slot.chunks.size() ||
        slot.chunk_used + need > (slot.chunks[slot.active_chunk] ? CHUNK_SIZE : 0)) {
        // Advance to the next chunk that fits; oversized draws get a dedicated chunk.
        if (slot.active_chunk < slot.chunks.size() && slot.chunk_used != 0) {
            slot.active_chunk++;
        }
        while (slot.active_chunk >= slot.chunks.size()) {
            slot.chunks.push_back(std::make_unique<u8[]>(std::max(need, CHUNK_SIZE)));
        }
        slot.chunk_used = 0;
        if (need > CHUNK_SIZE) {
            slot.chunks[slot.active_chunk] = std::make_unique<u8[]>(need);
        }
    }
    u8* const ptr = slot.chunks[slot.active_chunk].get() + slot.chunk_used;
    slot.chunk_used += need;
    return ptr;
}

void ThreadedRasterizer::RotateArenaSlots() {
    current_slot = nullptr;
    for (auto& slot : arena_slots) {
        if (slot.pending.load(std::memory_order_acquire) == 0) {
            slot.active_chunk = 0;
            slot.chunk_used = 0;
            current_slot = &slot;
            break;
        }
    }
}

bool ThreadedRasterizer::NoteShippedDraw(bool skippable) {
    const auto& fb = emu_pica.regs.internal.framebuffer.framebuffer;
    const u32 fb_pixels = (fb.width + 1) * (fb.height + 1);
    MarkGLWritten(fb.GetColorBufferPhysicalAddress(), fb_pixels * 4);
    MarkGLWritten(fb.GetDepthBufferPhysicalAddress(), fb_pixels * 4);
    const u32 color_target = fb.GetColorBufferPhysicalAddress();
    const bool repeat_target = IsRepeatTarget(color_target);
    RecordDrawTarget(color_target);
    if (skippable) {
        RecordTopTarget(fb.GetColorBufferPhysicalAddress());
        RecordTopTarget(fb.GetDepthBufferPhysicalAddress());
    }
    return !(skip_frame && (skippable || repeat_target));
}

void ThreadedRasterizer::WaitGpuIdle() {
    gpu_thread.RunSync([this] { real->WaitGpuIdle(); });
}

std::optional<RasterizerInterface::DrawArenaTicket> ThreadedRasterizer::BeginShippedDraw(
    u32 bytes, bool skippable, u32 ring_bytes) {
    {
        const auto& fb = emu_pica.regs.internal.framebuffer.framebuffer;
        const u32 fb_pixels = (fb.width + 1) * (fb.height + 1);
        MarkGLWritten(fb.GetColorBufferPhysicalAddress(), fb_pixels * 4);
        MarkGLWritten(fb.GetDepthBufferPhysicalAddress(), fb_pixels * 4);
        // The caller's flag is the top screen's own framebuffer, by height. A target an
        // earlier frame already drew into is skippable as well - see repeat_targets - and the
        // recording has to happen for every draw, whatever the skip state, or a target is
        // never learned. This is the gate the hardware renderer's draws pass through;
        // DrawTriangles below is the software one, and the two must agree.
        const u32 color_target = fb.GetColorBufferPhysicalAddress();
        const bool repeat_target = IsRepeatTarget(color_target);
        RecordDrawTarget(color_target);
        if (skippable) {
            RecordTopTarget(fb.GetColorBufferPhysicalAddress());
            RecordTopTarget(fb.GetDepthBufferPhysicalAddress());
        }
        if (skip_frame && (skippable || repeat_target)) {
            return DrawArenaTicket{nullptr, nullptr};
        }
    }
    if (ring_bytes != 0 && ring != nullptr && real != nullptr && real->WantsRingDraws() &&
        ring_bytes <= ring->size / 2) {
        // Straight into the GPU-visible ring, once, in the layout the draw reads. Only when
        // the ring has the room now: waiting here would be the emulation thread waiting on
        // the renderer, so a full ring takes the arena and the render thread's copy instead.
        std::scoped_lock lock{ring->mutex};
        const u32 off = ring->TryAlloc(ring_bytes);
        if (off != VertexRing::FULL) {
            const u64 end = ring->cursor;
            if (ring_inflight.empty()) {
                ring->consume_floor.store(end - ring_bytes, std::memory_order_release);
            }
            ring_inflight.emplace_back(end - ring_bytes, end);
            return DrawArenaTicket{ring->base + off, nullptr, true, off, end};
        }
    }
    if (current_slot == nullptr) {
        // No free frame arena (or none was ever opened): the draw still ships — frames that
        // render must be complete — through a buffer the shipment will own. Bounded: past the
        // budget, wait for the render thread to retire something and look for a freed slot.
        while (emergency_bytes.load(std::memory_order_relaxed) + bytes > EmergencyBudget &&
               gpu_thread.HasWorker()) {
            const u64 wait_t0 = Common::PipelineStats::NowUs();
            gpu_thread.WaitForRetire();
            Common::PipelineStats::gxw_arena_us.fetch_add(
                Common::PipelineStats::NowUs() - wait_t0, std::memory_order_relaxed);
            RotateArenaSlots();
            if (current_slot != nullptr) {
                return DrawArenaTicket{SlotAlloc(*current_slot, bytes), current_slot};
            }
        }
        emergency_arena = std::make_unique<u8[]>(std::max(bytes, 1u));
        emergency_arena_bytes = std::max(bytes, 1u);
        emergency_bytes.fetch_add(emergency_arena_bytes, std::memory_order_relaxed);
        return DrawArenaTicket{emergency_arena.get(), nullptr};
    }
    return DrawArenaTicket{SlotAlloc(*current_slot, bytes), current_slot};
}

void ThreadedRasterizer::PostInterruptAfterQueue(Service::GSP::InterruptId id, u64 delay_ns) {
    if (early_completion && early_interrupt_poster) {
        // The engine's own modelled timing rather than the render thread's backlog. The work
        // this acknowledges still rides the queue, and everything the renderer does - presents
        // included - stays ordered behind it.
        early_interrupt_poster(id, delay_ns);
        return;
    }
    // Timed: the gap between enqueueing this and the render thread reaching it is exactly what
    // a guest waiting on the completion has to sit through. See Common::PipelineStats.
    const u64 queued_at = Common::PipelineStats::NowUs();
    gpu_thread.Enqueue({.run = [this, id, delay_ns, queued_at] {
        Common::PipelineStats::interrupt_lag_us.fetch_add(
            Common::PipelineStats::NowUs() - queued_at, std::memory_order_relaxed);
        Common::PipelineStats::interrupt_count.fetch_add(1, std::memory_order_relaxed);
        deferred_interrupt_poster(id, delay_ns);
    }});
}

void ThreadedRasterizer::ShipDraw(Pica::DrawPayload&& payload, void* slot) {
    Common::PipelineStats::gxc_ships.fetch_add(1, std::memory_order_relaxed);
    const u64 ship_t0 = Common::EmuProf::enabled ? Common::PipelineStats::NowUs() : 0;
    SCOPE_EXIT({
        if (Common::EmuProf::enabled) {
            Common::EmuProf::Add("ship draw (total)", Common::PipelineStats::NowUs() - ship_t0);
        }
    });
#ifdef CITRA_TRACE_PROBES
    {
        static const bool trace = std::getenv("AZAHAR_DRAW_TRACE") != nullptr;
        static std::atomic<u32> n{0};
        if (trace && n.fetch_add(1) < 3000) {
            LOG_INFO(HW_GPU, "DRAWTRACE ship hw={}", payload.hw);
        }
    }
#endif // CITRA_TRACE_PROBES
    namespace EP = Common::EmuProf;
    static const bool step_prof = EP::enabled && !GxmRenderer::GxmFlag("noregprof");
    u64 prof_t = step_prof ? Common::PipelineStats::NowUs() : 0;
    const auto prof = [&](const char* key) {
        if (step_prof) {
            const u64 now = Common::PipelineStats::NowUs();
            EP::Add(key, now - prof_t);
            prof_t = now;
        }
    };
    Shipment* const shipment = TakeShipment();
    prof(" .ship: take");
    shipment->has_draw = true;
    shipment->draw = payload;
    if (shipment->draw.arena_inline) {
        shipment->draw.arena = reinterpret_cast<const u8*>(shipment->draw.immediate_input.data());
    }
    shipment->owned_arena = std::move(emergency_arena);
    shipment->owned_arena_bytes = emergency_arena_bytes;
    emergency_arena_bytes = 0;
    CollectState(*shipment);
    prof(" .ship: collect");

    shipment->arena_slot = static_cast<ArenaSlot*>(slot);
    if (shipment->arena_slot) {
        shipment->arena_slot->pending.fetch_add(1, std::memory_order_acq_rel);
    }
    // Two pointers: libstdc++'s std::function keeps a closure that small inline (8 bytes on the
    // Vita), where the three-pointer one was a malloc here and a free on the render thread for
    // every draw.
    QueuePreshade(shipment);
    const auto op = [this, shipment] {
        const u64 op_t0 = Common::PipelineStats::StatUs();
        ApplyShipment(mirror_pica, *shipment);
        if (op_t0 != 0) {
            Common::PipelineStats::rt_apply_us.fetch_add(
                Common::PipelineStats::StatUs() - op_t0, std::memory_order_relaxed);
        }
        SCOPE_EXIT({
            if (op_t0 != 0) {
                Common::PipelineStats::rt_op_us.fetch_add(
                    Common::PipelineStats::StatUs() - op_t0, std::memory_order_relaxed);
                Common::PipelineStats::rt_ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
        const u32 watch = GuestWatch::Read();
        // Geometry-shader draws are never offered to the hardware vertex path; the GPU takes
        // the ones a fused VS + GS program covers (AccelerateGsDraw), the rest shade here.
        bool drawn = false;
        if (shipment->draw.hw) {
            drawn = real->AccelerateShippedDraw(shipment->draw);
        } else if (mirror_pica.regs.internal.pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes &&
                   mirror_pica.GetPrimitiveAssembler().IsEmpty()) {
            drawn = real->AccelerateGsDraw(shipment->draw);
        }
        if (!drawn) {
            const u64 sw_start = Common::PipelineStats::StatUs();
            RunSoftwareDraw(*shipment);
            Common::PipelineStats::sw_shaded_us.fetch_add(
                Common::PipelineStats::StatUs() - sw_start, std::memory_order_relaxed);
            Common::PipelineStats::sw_shaded_draws.fetch_add(1, std::memory_order_relaxed);
            if (!shipment->draw.hw) {
                Common::PipelineStats::sw_shaded_not_offered.fetch_add(1,
                                                                       std::memory_order_relaxed);
            }
        }
        if (shipment->draw.in_ring) {
            RingDrawConsumed(shipment->draw.ring_end);
        }
        {
            const auto& fb = mirror_pica.regs.internal.framebuffer.framebuffer;
            GuestWatch::Check(watch, "draw", fb.GetColorBufferPhysicalAddress(),
                              fb.GetDepthBufferPhysicalAddress(),
                              (fb.GetWidth() << 16) | fb.GetHeight());
        }
        ReleaseShipment(shipment);
    };
    static_assert(sizeof(op) <= 2 * sizeof(void*) && std::is_trivially_copyable_v<decltype(op)>);
    gpu_thread.EnqueueFromProducer({.run = op});
    prof(" .ship: enqueue");
}

void ThreadedRasterizer::DrawTriangles() {
    if (ring ? ranges.empty() : batch.empty()) {
        return;
    }

    // Frameskip is cost-based per render target: the bottom screen's framebuffer (240x320
    // against the top's 240x400) carries cheap 2D that titles often draw exactly once — a HUD
    // dropped with its frame stays black forever. Only draws into the expensive, continuously
    // redrawn top-screen targets are skippable.
    const auto& fb = emu_pica.regs.internal.framebuffer.framebuffer;
    const u32 fb_height = fb.height + 1;
    const u32 color_target = fb.GetColorBufferPhysicalAddress();
    // Either the target is the top screen's own framebuffer, or it is one this phase has
    // already drawn into at least twice - see the note on repeat_targets. Recording happens
    // whatever the skip state, so a target that only starts repeating during a skipped run
    // is still recognised.
    const bool repeat_target = IsRepeatTarget(color_target);
    RecordDrawTarget(color_target);
    const bool skippable_target = fb_height > 350 || repeat_target;

    if (fb_height > 350) {
        // Remember the target so clears aimed at it can be dropped along with the draws.
        RecordTopTarget(fb.GetColorBufferPhysicalAddress());
        RecordTopTarget(fb.GetDepthBufferPhysicalAddress());
    }

    if (skip_frame && skippable_target) {
        // Nothing ships for a skipped batch — not even state. The dirty bits keep accumulating
        // on the emulation side and the next shipment carries them all at once; with the guest
        // uncapped, per-batch state ops alone were enough to saturate the render thread. In ring
        // mode the cursor simply rewinds over the batch's vertices: nothing referenced them yet.
        if (ring) {
            ring->Rewind(batch_start_cursor, batch_start_offset);
            ranges.clear();
        } else {
            batch.clear();
        }
        return;
    }

    Shipment* const shipment = TakeShipment();
    if (ring) {
        std::swap(shipment->ranges, ranges);
        ranges.clear();
        batch_start_cursor = ring->cursor;
        batch_start_offset = ring->offset;
    } else {
        // The batch swaps into the shipment and a pooled buffer (with grown capacity) swaps in
        // behind it; the render thread returns the shipment's buffer to the pool afterwards, so
        // no per-batch reallocation ever happens in steady state.
        shipment->vertices = std::move(batch);
        batch = TakeBuffer();
    }

    CollectState(*shipment);
    QueuePreshade(shipment);

    gpu_thread.Enqueue({.run = [this, shipment] {
        ApplyShipment(mirror_pica, *shipment);
        if (!shipment->ranges.empty()) {
            for (const auto& range : shipment->ranges) {
                real->DrawRingRange(range.first, range.count, range.end_pos);
            }
        } else if (!shipment->vertices.empty()) {
            for (std::size_t i = 0; i + 2 < shipment->vertices.size(); i += 3) {
                real->AddTriangle(shipment->vertices[i], shipment->vertices[i + 1],
                                  shipment->vertices[i + 2]);
            }
            real->DrawTriangles();
            ReturnBuffer(std::move(shipment->vertices));
        }
        ReleaseShipment(shipment);
    }});
}

void ThreadedRasterizer::MarkGLWritten(PAddr addr, u32 size) {
    if (size == 0) {
        return;
    }
    if (addr == last_marked_addr && size == last_marked_size) {
        return; // The framebuffer registers rarely move between draws.
    }
    last_marked_addr = addr;
    last_marked_size = size;
    const PAddr begin = std::max(addr, GL_WRITTEN_BASE);
    const u64 end = std::min<u64>(u64(addr) + size, GL_WRITTEN_END);
    if (begin >= end) {
        return;
    }
    const u32 first = (begin - GL_WRITTEN_BASE) >> GL_WRITTEN_PAGE_BITS;
    const u32 last = static_cast<u32>(end - 1 - GL_WRITTEN_BASE) >> GL_WRITTEN_PAGE_BITS;
    // Word-granular fill: transfers mark hundreds of pages per call and this measured 5% of
    // the emulation thread when done bit by bit.
    const u32 first_word = first >> 6;
    const u32 last_word = last >> 6;
    const u64 first_mask = ~0ull << (first & 63);
    const u64 last_mask = ~0ull >> (63 - (last & 63));
    if (first_word == last_word) {
        gl_written_pages[first_word] |= first_mask & last_mask;
        return;
    }
    gl_written_pages[first_word] |= first_mask;
    for (u32 word = first_word + 1; word < last_word; ++word) {
        gl_written_pages[word] = ~0ull;
    }
    gl_written_pages[last_word] |= last_mask;
}

bool ThreadedRasterizer::TestGLWritten(PAddr addr, u32 size) const {
    if (size == 0) {
        return false;
    }
    if (addr < GL_WRITTEN_BASE || u64(addr) + size > GL_WRITTEN_END) {
        return true; // Outside the tracked window: be conservative.
    }
    const u32 first = (addr - GL_WRITTEN_BASE) >> GL_WRITTEN_PAGE_BITS;
    const u32 last = (addr + size - 1 - GL_WRITTEN_BASE) >> GL_WRITTEN_PAGE_BITS;
    const u32 first_word = first >> 6;
    const u32 last_word = last >> 6;
    const u64 first_mask = ~0ull << (first & 63);
    const u64 last_mask = ~0ull >> (63 - (last & 63));
    if (first_word == last_word) {
        return (gl_written_pages[first_word] & first_mask & last_mask) != 0;
    }
    if (gl_written_pages[first_word] & first_mask) {
        return true;
    }
    for (u32 word = first_word + 1; word < last_word; ++word) {
        if (gl_written_pages[word] != 0) {
            return true;
        }
    }
    return (gl_written_pages[last_word] & last_mask) != 0;
}

ThreadedRasterizer::Shipment* ThreadedRasterizer::TakeShipment() {
    Shipment* head = free_head.load(std::memory_order_acquire);
    while (head != nullptr &&
           !free_head.compare_exchange_weak(head, head->next_free, std::memory_order_acquire,
                                            std::memory_order_acquire)) {
    }
    if (head != nullptr) {
        head->Reset();
        return head;
    }
    shipment_pool.push_back(std::make_unique<Shipment>());
    return shipment_pool.back().get();
}

void ThreadedRasterizer::ReleaseShipment(Shipment* shipment) {
    if (shipment->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    if (shipment->arena_slot) {
        shipment->arena_slot->pending.fetch_sub(1, std::memory_order_acq_rel);
    }
    if (shipment->owned_arena_bytes) {
        emergency_bytes.fetch_sub(shipment->owned_arena_bytes, std::memory_order_relaxed);
    }
    // The two large per-shipment buffers - a program copy (~20 KiB) and the lighting LUT block
    // (24 KiB) - are freed here rather than kept for reuse: the free list holds every shipment
    // that was ever in flight (~950 with the GX queue's frame of overlap), and nearly all of
    // them end up having carried a LUT change, which was ~23 MB of heap that never came back
    // (out-of-memory while walking the village, 2026-11-14).
    shipment->vs.code.reset();
    shipment->gs.code.reset();
    shipment->lighting_luts.reset();
    // The same for the shaded triangles: a geometry-shader draw's vertices (~20 KiB) used to
    // stay as capacity in whichever shipment carried it, and over a long walk nearly all of the
    // ~1600 pooled shipments had - 32 MB, the rest of the heap growth that ran the village out
    // of memory. Kept only when small.
    constexpr std::size_t KeepVertices = 64;
    if (shipment->vertices.capacity() > KeepVertices) {
        std::vector<Pica::OutputVertex>().swap(shipment->vertices);
    }
    if (shipment->ranges.capacity() > KeepVertices) {
        std::vector<VertexRingRange>().swap(shipment->ranges);
    }
    // Last: the emulation thread may take and reset the shipment right after.
    ReturnShipment(shipment);
}

ThreadedRasterizer::~ThreadedRasterizer() {
    if (shade_thread.joinable()) {
        shade_quit.store(true, std::memory_order_release);
        shade_thread.join();
    }
}

void ThreadedRasterizer::EnablePreshade(Memory::MemorySystem& memory) {
    // `nopreshade` (diagnostic flag), or ux0:data/mh4u/nopreshade in any build.
    std::FILE* off = std::fopen(VITA_DATA_DIR "/nopreshade", "rb");
    if (off != nullptr) {
        std::fclose(off);
    }
    if (preshade || off != nullptr || GxmRenderer::GxmFlag("nopreshade")) {
        LOG_INFO(Render, "pre-shading off");
        return;
    }
    shade_pica = std::make_unique<Pica::PicaCore>(memory, nullptr);
    shade_ring = std::make_unique<Shipment*[]>(ShadeRingSize);
    preshade = true;
    shade_thread = std::thread([this] { PreshadeLoop(); });
}

void ThreadedRasterizer::QueuePreshade(Shipment* shipment) {
    if (!preshade) {
        return;
    }
    // Point-mode geometry-shader draws in software: the GS does not write its own uniforms
    // (FixedPrimitive does), so the two mirrors stay identical whichever thread shades.
    const auto& pipeline = emu_pica.regs.internal.pipeline;
    const bool eligible = shipment->has_draw && !shipment->draw.hw && !shipment->draw.immediate &&
                          pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes &&
                          pipeline.gs_config.mode == Pica::PipelineRegs::GSMode::Point;
    shipment->shade_state.store(eligible ? ShadePending : ShadeNone, std::memory_order_relaxed);
    shipment->refs.store(2, std::memory_order_relaxed);
    const u32 tail = shade_tail.load(std::memory_order_relaxed);
    if (tail - shade_head.load(std::memory_order_acquire) >= ShadeRingSize) [[unlikely]] {
        const u64 t0 = Common::PipelineStats::NowUs();
        while (tail - shade_head.load(std::memory_order_acquire) >= ShadeRingSize) {
            std::this_thread::yield(); // full: the worker is thousands of shipments behind
        }
        Common::PipelineStats::gxw_shade_us.fetch_add(Common::PipelineStats::NowUs() - t0,
                                                      std::memory_order_relaxed);
    }
    shade_ring[tail % ShadeRingSize] = shipment;
    // No wake-up: the worker polls (PreshadeLoop). Waking it per shipment was a kernel call
    // each, ~12 us on the emulation thread thousands of times a second.
    shade_tail.store(tail + 1, std::memory_order_release);
}

namespace {
/// gsdump (diag): the geometry-shader draws the pre-shade worker runs, for planning a GPU
/// path for them. Each distinct program pair once to ux0:data/mh4u/gsdump_<key>.bin (the
/// whole register file, both programs and swizzles, both uniform sets, default attributes,
/// the draw's arena bytes and its output triangles), and from 30 s into the run one log
/// line per draw for the next 600 draws.
void GsDump(Pica::PicaCore& pica, const Pica::DrawPayload& d,
            const std::vector<Pica::OutputVertex>& out, bool ok) {
    static const bool enabled = GxmRenderer::GxmFlag("gsdump");
    if (!enabled) {
        return;
    }
    auto& gs = pica.gs_setup;
    auto& vs = pica.vs_setup;
    const auto& r = pica.regs.internal;
    const u64 key = gs.GetProgramCodeHash() ^ (gs.GetSwizzleDataHash() << 1) ^
                    (u64{r.gs.main_offset} << 48) ^ (vs.GetProgramCodeHash() * 3);
    static std::vector<u64> seen;
    if (seen.size() < 32 && std::find(seen.begin(), seen.end(), key) == seen.end()) {
        seen.push_back(key);
        char name[96];
        std::snprintf(name, sizeof(name), VITA_DATA_DIR "/gsdump_%016llx.bin",
                      static_cast<unsigned long long>(key));
        if (std::FILE* f = std::fopen(name, "wb")) {
            const u32 hdr[12] = {0x50534447u, 1, sizeof(pica.regs.internal),
                                 static_cast<u32>(sizeof(Pica::ProgramCode)),
                                 static_cast<u32>(sizeof(Pica::SwizzleData)),
                                 static_cast<u32>(sizeof(Pica::Uniforms)),
                                 static_cast<u32>(sizeof(pica.input_default_attributes)),
                                 d.layout.total, d.num_vertices,
                                 (u32{d.is_indexed} << 1) | u32{d.index_u16} | (u32{ok} << 2),
                                 static_cast<u32>(out.size()),
                                 static_cast<u32>(sizeof(Pica::OutputVertex))};
            std::fwrite(hdr, sizeof(hdr), 1, f);
            const u32 extra[4] = {d.vertex_min, d.vertex_max, d.vertex_offset, d.layout.index_offset};
            std::fwrite(extra, sizeof(extra), 1, f);
            std::fwrite(d.layout.loader_offset.data(), sizeof(u32), 12, f);
            std::fwrite(&pica.regs.internal, sizeof(pica.regs.internal), 1, f);
            std::fwrite(gs.GetProgramCode().data(), sizeof(Pica::ProgramCode), 1, f);
            std::fwrite(gs.GetSwizzleData().data(), sizeof(Pica::SwizzleData), 1, f);
            std::fwrite(vs.GetProgramCode().data(), sizeof(Pica::ProgramCode), 1, f);
            std::fwrite(vs.GetSwizzleData().data(), sizeof(Pica::SwizzleData), 1, f);
            std::fwrite(&gs.uniforms, sizeof(Pica::Uniforms), 1, f);
            std::fwrite(&vs.uniforms, sizeof(Pica::Uniforms), 1, f);
            std::fwrite(&pica.input_default_attributes, sizeof(pica.input_default_attributes), 1, f);
            if (d.arena != nullptr && d.layout.total != 0) {
                std::fwrite(d.arena, d.layout.total, 1, f);
            }
            if (!out.empty()) {
                std::fwrite(out.data(), sizeof(Pica::OutputVertex), out.size(), f);
            }
            std::fclose(f);
            LOG_INFO(Render, "gsdump: wrote {}", name);
        }
    }
    static int logged = 0;
    static u64 last_gsf = 0, last_vsf = 0;
    static std::FILE* draws = nullptr;
    if (logged < 600 && sceKernelGetProcessTimeWide() > 30'000'000ull) {
        if (draws == nullptr) {
            draws = std::fopen(VITA_DATA_DIR "/gsdraws.txt", "w");
        }
        logged++;
        const u64 gsf = gs.FloatUniformFingerprint(), vsf = vs.FloatUniformFingerprint();
        if (draws != nullptr) {
            std::fprintf(draws, "key %016llx nv %u idx %d tris %u ok %d gsf %s vsf %s fixnum %u stride %u\n",
                         static_cast<unsigned long long>(key), d.num_vertices, d.is_indexed ? 1 : 0,
                         static_cast<unsigned>(out.size() / 3), ok ? 1 : 0,
                         gsf != last_gsf ? "chg" : "same", vsf != last_vsf ? "chg" : "same",
                         static_cast<unsigned>(r.pipeline.gs_config.fixed_vertex_num_minus_1.Value() + 1),
                         static_cast<unsigned>(r.pipeline.gs_config.stride_minus_1.Value() + 1));
            if (logged == 600) {
                std::fclose(draws);
                draws = nullptr;
            }
        }
        last_gsf = gsf;
        last_vsf = vsf;
    }
}
} // namespace

void ThreadedRasterizer::PreshadeLoop() {
    // Core 0, or core 3 when the GX queue (on core 0) is in use.
    Common::SetCurrentThreadRole(Common::gx_fifo ? Common::ThreadRole::RasterWorker
                                                 : Common::ThreadRole::ShaderCompiler);
    Common::SetCurrentThreadName("PreShade");
#ifdef __vita__
    // Below the DSP worker on the same core: the emulation thread waits for each DSP tick, and
    // a screen of text glyphs once kept this worker busy enough to hold those up by ~20 ms.
    // (The render thread shades a draw itself when this one falls behind.)
    // On core 3 (the GX queue's case) it sits just below the audio sink and above the other
    // threads there: at USER_DEFAULT + 8 it fell behind on the quest counter (~450 glyph and
    // sprite draws a frame) and the render thread shaded a third of the draws itself
    // (2026-11-14). It sleeps when its ring is empty. `preshadelow` restores the old priority.
    static const bool low = !Common::gx_fifo || GxmRenderer::GxmFlag("preshadelow");
    sceKernelChangeThreadPriority(0, low ? SCE_KERNEL_PROCESS_PRIORITY_USER_DEFAULT + 8
                                         : SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH + 1);
#endif
    for (;;) {
        const u32 head = shade_head.load(std::memory_order_relaxed);
        if (shade_tail.load(std::memory_order_acquire) == head) {
            // Empty: spin a little (the next shipment is usually microseconds away), then sleep.
            bool got = false;
            for (int i = 0; i < 200 && !got; i++) {
                got = shade_tail.load(std::memory_order_acquire) != head;
            }
            if (!got) {
                if (shade_quit.load(std::memory_order_acquire)) {
                    return;
                }
                // Polled rather than woken: shipments wait here at most this long, while the
                // render thread runs most of a frame behind the emulation thread anyway.
                std::this_thread::sleep_for(std::chrono::microseconds(150));
            }
            continue;
        }
        Shipment* const shipment = shade_ring[head % ShadeRingSize];
        shade_head.store(head + 1, std::memory_order_release);
        // Every shipment, in order, so this mirror's vertex-side state is the render mirror's
        // at the same draw.
        const u64 apply_t0 = Common::PipelineStats::StatUs();
        ApplyShipment(*shade_pica, *shipment, false);
        if (apply_t0 != 0) {
            Common::PipelineStats::psw_apply_us.fetch_add(
                Common::PipelineStats::StatUs() - apply_t0, std::memory_order_relaxed);
        }
        u32 expected = ShadePending;
        if (!shade_quit.load(std::memory_order_relaxed) &&
            shipment->shade_state.compare_exchange_strong(
                               expected, ShadeRunning, std::memory_order_acq_rel)) {
            shipment->shade_ok = shade_pica->ShadeShippedDraw(shipment->draw, shipment->vertices);
            GsDump(*shade_pica, shipment->draw, shipment->vertices, shipment->shade_ok);
            shipment->shade_state.store(ShadeDone, std::memory_order_release);
        }
        ReleaseShipment(shipment);
    }
}

void ThreadedRasterizer::RunSoftwareDraw(Shipment& shipment) {
    namespace PS = Common::PipelineStats;
    u32 state = shipment.shade_state.load(std::memory_order_acquire);
    if (state == ShadePending &&
        shipment.shade_state.compare_exchange_strong(state, ShadeStolen,
                                                     std::memory_order_acq_rel)) {
        PS::preshade_stolen.fetch_add(1, std::memory_order_relaxed);
        const u64 t0 = PS::StatUs();
        mirror_pica.RunShippedDraw(shipment.draw);
        PS::preshade_local_us.fetch_add(PS::StatUs() - t0, std::memory_order_relaxed);
        return;
    }
    if (state == ShadeRunning) {
        // The worker is on it: wait for its triangles. Shading it again here was thought to
        // cost about what waiting would (true for 1-4 point HUD sprites), but MH4U's monster
        // and particle GS draws are thousands of triangles: near an active herd in play the
        // render thread spent ~600 of ~950 ms a second in software draws and became the limit
        // (20 fps, 2026-11-15). ux0:data/mh4u/preshadesteal (or flag) shades it here again
        // as before.
        static const bool steal_running = [] {
            if (std::FILE* f = std::fopen(VITA_DATA_DIR "/preshadesteal", "rb")) {
                std::fclose(f);
                return true;
            }
            return GxmRenderer::GxmFlag("preshadesteal");
        }();
        if (steal_running) {
            PS::preshade_stolen.fetch_add(1, std::memory_order_relaxed);
            const u64 t0 = PS::StatUs();
            mirror_pica.RunShippedDraw(shipment.draw);
            PS::preshade_steal_running_us.fetch_add(PS::StatUs() - t0,
                                                    std::memory_order_relaxed);
            return;
        }
        PS::preshade_waited.fetch_add(1, std::memory_order_relaxed);
        const u64 t0 = PS::StatUs();
        while ((state = shipment.shade_state.load(std::memory_order_acquire)) == ShadeRunning) {
            std::this_thread::yield();
        }
        PS::preshade_wait_us.fetch_add(PS::StatUs() - t0, std::memory_order_relaxed);
    }
    // The worker's triangles stand in for this mirror's shading only when its primitive
    // assembler is as empty as the worker's was: a GS draw then starts and ends with it empty.
    if (state == ShadeDone && shipment.shade_ok &&
        mirror_pica.GetPrimitiveAssembler().IsEmpty()) {
        PS::preshade_used.fetch_add(1, std::memory_order_relaxed);
        const u64 t0 = PS::StatUs();
        const auto& v = shipment.vertices;
        for (std::size_t i = 0; i + 2 < v.size(); i += 3) {
            real->AddTriangle(v[i], v[i + 1], v[i + 2]);
        }
        const u64 t1 = PS::StatUs();
        real->DrawTriangles();
        PS::preshade_feed_us.fetch_add(t1 - t0, std::memory_order_relaxed);
        PS::preshade_draw_us.fetch_add(PS::StatUs() - t1, std::memory_order_relaxed);
        PS::preshade_tris.fetch_add(static_cast<u32>(v.size() / 3), std::memory_order_relaxed);
        return;
    }
    if (state == ShadeDone) {
        PS::preshade_refused.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 other_t0 = PS::StatUs();
    mirror_pica.RunShippedDraw(shipment.draw);
    PS::preshade_other_us.fetch_add(PS::StatUs() - other_t0, std::memory_order_relaxed);
}

void ThreadedRasterizer::ReturnShipment(Shipment* shipment) {
    Shipment* head = free_head.load(std::memory_order_relaxed);
    do {
        shipment->next_free = head;
    } while (!free_head.compare_exchange_weak(head, shipment, std::memory_order_release,
                                              std::memory_order_relaxed));
}

void ThreadedRasterizer::FlushAll() {
    gpu_thread.RunSync([this] { real->FlushAll(); });
}

void ThreadedRasterizer::FlushRegion(PAddr addr, u32 size) {
    // A flush writes GL-rendered content back to guest memory before the caller reads it. If
    // the GL never wrote inside this range there is nothing to wait for; the page map is
    // conservative, so a clear range is proof.
    if (!TestGLWritten(addr, size)) {
        return;
    }
    if (unsafe_async_flush) {
        // The caller reads stale guest memory for up to the queue latency; see
        // SetUnsafeAsyncFlush.
        gpu_thread.Enqueue({.run = [this, addr, size] { real->FlushRegion(addr, size); }});
        return;
    }
    gpu_thread.RunSync([this, addr, size] { real->FlushRegion(addr, size); });
}

void ThreadedRasterizer::InvalidateRegion(PAddr addr, u32 size) {
    InvalidateVertexCache(addr, size);
    // Invalidation is an ordering requirement, not a synchronous one: the caches must drop
    // stale surfaces before the *next* GL op that could read them, and that op sits behind this
    // one in the same queue. Fire and forget.
    gpu_thread.Enqueue({.run = [this, addr, size] { real->InvalidateRegion(addr, size); }});
}

void ThreadedRasterizer::InvalidateGuestFlushedRegion(PAddr addr, u32 size) {
    // Decided where the answer lives: the cache's dirty regions, on the render thread, say
    // byte-exactly which pixels exist only host-side. Ordered like any invalidate.
    gpu_thread.Enqueue(
        {.run = [this, addr, size] { real->InvalidateGuestFlushedRegion(addr, size); }});
}

void ThreadedRasterizer::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    InvalidateVertexCache(addr, size);
    if (!TestGLWritten(addr, size)) {
        InvalidateRegion(addr, size);
        return;
    }
    if (unsafe_async_flush) {
        gpu_thread.Enqueue(
            {.run = [this, addr, size] { real->FlushAndInvalidateRegion(addr, size); }});
        return;
    }
    gpu_thread.RunSync([this, addr, size] { real->FlushAndInvalidateRegion(addr, size); });
}

void ThreadedRasterizer::InvalidateVertexCache(PAddr addr, u32 size) {
    auto& cache = Pica::GetVertexCache();
    if (!cache.enabled || size == 0) {
        return;
    }
    if (const u8* host = emu_pica.GuestMemory().GetPhysicalPointer(addr)) {
        cache.InvalidateHost(host, size);
    }
}

void ThreadedRasterizer::ClearAll(bool flush) {
    gpu_thread.RunSync([this, flush] { real->ClearAll(flush); });
}

} // namespace VideoCore
