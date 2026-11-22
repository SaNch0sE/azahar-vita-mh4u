// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// RasterizerSgxd's profiling, in the diagnostics build only: vprof's recording of a frame (each
// draw's memory and state, the passes' and transfers' results, the renderer's state at the
// frame's start), its replay's restore and readback, and perflog's notes.

#ifdef VITA_DIAGNOSTICS

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"
#include "video_core/renderer_sgxd/vprof.h"

namespace SgxdRenderer {

using FramebufferRegs = Pica::FramebufferRegs;

// vprof: the guest memory a recorded frame's draw reads, and the draw with its state.
void RasterizerSgxd::VprofDraw(bool is_indexed, bool immediate, u32 count) {
    vprof_draw_noted = false;
    if (!Vprof::InOp()) {
        return;
    }
    vprof_draw_noted = true;
    vprof_draws++;
    using Vprof::Mem;
    const auto& fb = regs.framebuffer.framebuffer;
    const u32 pass_draw = backend.PassDraws() + 1;
    if (pass_draw == 1) {
        const u32 pixels = fb.GetWidth() * fb.GetHeight();
        Vprof::Memory(Mem::Colour, fb.GetColorBufferPhysicalAddress(),
                      pixels * Pica::FramebufferRegs::BytesPerColorPixel(fb.color_format));
        if (fb.GetDepthBufferPhysicalAddress() != 0) {
            Vprof::Memory(Mem::Depth, fb.GetDepthBufferPhysicalAddress(),
                          pixels * Pica::FramebufferRegs::BytesPerDepthPixel(fb.depth_format));
        }
    }
    if (!immediate) {
        const auto info = AnalyzeVertexArray(is_indexed);
        const auto& va = regs.pipeline.vertex_attributes;
        const PAddr base = va.GetPhysicalBaseAddress();
        for (u32 i = 0; i < 12; i++) {
            const auto& loader = va.attribute_loaders[i];
            if (loader.component_count == 0 || loader.byte_count == 0) {
                continue;
            }
            const u32 first = info.vs_input_index_min * loader.byte_count;
            const u32 end = (info.vs_input_index_max + 1) * loader.byte_count;
            Vprof::Memory(Mem::Vertices, base + loader.data_offset + first, end - first);
        }
        if (is_indexed) {
            const auto& ia = regs.pipeline.index_array;
            Vprof::Memory(Mem::Indices, base + ia.offset, count * (ia.format ? 2 : 1));
        }
    }
    const auto textures = regs.texturing.GetTextures();
    for (u32 unit = 0; unit < 3; unit++) {
        const auto& t = textures[unit];
        if (!t.enabled) {
            continue;
        }
        // NibblesPerPixel has no ETC formats (UNIMPLEMENTED, a <Critical> line per draw):
        // ETC1 is 4 bits a pixel, ETC1A4 8.
        using Format = Pica::TexturingRegs::TextureFormat;
        const u32 nibbles = t.format == Format::ETC1     ? 1
                            : t.format == Format::ETC1A4 ? 2
                                                         : Pica::TexturingRegs::NibblesPerPixel(t.format);
        u32 bytes = 0;
        for (u32 level = 0; level <= t.config.lod.max_level; level++) {
            const u32 w = std::max(t.config.width >> level, 8u);
            const u32 h = std::max(t.config.height >> level, 8u);
            bytes += w * h * nibbles / 2;
        }
        using Type = Pica::TexturingRegs::TextureConfig::TextureType;
        const auto type = t.config.type.Value();
        if (unit == 0 && (type == Type::TextureCube || type == Type::ShadowCube)) {
            for (u32 face = 0; face < 6; face++) {
                Vprof::Memory(Mem::Texture,
                              regs.texturing.GetCubePhysicalAddress(
                                  static_cast<Pica::TexturingRegs::CubeFace>(face)),
                              bytes);
            }
        } else {
            Vprof::Memory(Mem::Texture, t.config.GetPhysicalAddress(), bytes);
        }
    }
    Vprof::Draw(pica, passes, pass_draw, count);
}

void RasterizerSgxd::ReplayEndPass() {
    FlushImmediate();
    if (backend.InPass()) {
        EndPass("vprof replay");
    }
}

namespace {
constexpr u32 VprofTag(const char (&t)[5]) {
    return u32(u8(t[0])) | u32(u8(t[1])) << 8 | u32(u8(t[2])) << 16 | u32(u8(t[3])) << 24;
}

/// A colour buffer's guest bytes (the PICA's 8x8 tiles, x first inside) as A8B8G8R8 rows, R the
/// lowest byte, row y memory row y: what the surfaces of earlier builds held.
std::vector<u8> DecodeColour(const u8* guest, u32 width, u32 height,
                             FramebufferRegs::ColorFormat format) {
    using CF = FramebufferRegs::ColorFormat;
    const u32 bpp = FramebufferRegs::BytesPerColorPixel(format);
    const auto five = [](u32 v) { return (v << 3 | v >> 2) & 0xff; };
    std::vector<u8> out(std::size_t(width) * height * 4);
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            const u32 i = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 |
                          (y & 4) << 3;
            const u8* b = guest + (((y / 8) * (width / 8) + x / 8) * 64 + i) * bpp;
            const u32 v = b[0] | (bpp > 1 ? b[1] << 8 : 0);
            u32 r = 0, g = 0, bl = 0, a = 255;
            switch (format) {
            case CF::RGBA8:
                r = b[3], g = b[2], bl = b[1], a = b[0];
                break;
            case CF::RGB8:
                r = b[2], g = b[1], bl = b[0];
                break;
            case CF::RGB5A1:
                r = five(v >> 11 & 31), g = five(v >> 6 & 31), bl = five(v >> 1 & 31);
                a = v & 1 ? 255 : 0;
                break;
            case CF::RGB565:
                r = five(v >> 11), g = (v >> 5 & 63) << 2 | (v >> 9 & 3), bl = five(v & 31);
                break;
            default:
                r = (v >> 12) * 17, g = (v >> 8 & 15) * 17, bl = (v >> 4 & 15) * 17,
                a = (v & 15) * 17;
                break;
            }
            u8* o = &out[(std::size_t(y) * width + x) * 4];
            o[0] = static_cast<u8>(r), o[1] = static_cast<u8>(g), o[2] = static_cast<u8>(bl);
            o[3] = static_cast<u8>(a);
        }
    }
    return out;
}
} // namespace

void RasterizerSgxd::VprofFrameStart() {
    // The frame's passes start from here: a pass left open by the frame before ends first, so
    // the state below is all a pass of the frame can start from.
    FlushImmediate();
    if (backend.InPass()) {
        vprof_frame_start = true;
        EndPass("vprof: the recorded frame starts");
        vprof_frame_start = false;
    }
    backend.RenderIdle("vprof: the recorded frame's start");
    std::vector<u32> w{1, clear_order, clear_count};
    for (u32 i = 0; i < clear_count; i++) {
        const ClearedRange& c = cleared[i];
        w.insert(w.end(), {c.start, c.end, c.value, c.bytes, c.order});
    }
    // No colour surfaces (the generation count and none): colour buffers are guest memory,
    // which each pass's first draw records (VprofDraw, Mem::Colour).
    w.push_back(0);
    w.push_back(0);
    w.push_back(static_cast<u32>(depth_stores.size()));
    for (const DepthStore& d : depth_stores) {
        const u32 bytes = d.store.width * d.store.height * (d.store.stencil ? 5 : 4);
        w.insert(w.end(), {d.addr, d.width, d.height, d.format, d.valid ? 1u : 0u, bytes,
                           Vprof::FrameBlob(d.store.memory.cpu, bytes)});
    }
    Vprof::FrameRecord(VprofTag("RSTA"), w);
    LOG_INFO(Render, "vprof: the frame starts from {} clears and {} depth stores", clear_count,
             depth_stores.size());
}

void RasterizerSgxd::ReplayRestore(const VprofState& state, bool retile) {
    FlushImmediate();
    if (backend.InPass()) {
        vprof_frame_start = true;
        EndPass("vprof replay: the frame's start");
        vprof_frame_start = false;
    }
    backend.RenderIdle("vprof replay: the frame's start");
    for (const DepthStore& d : depth_stores) {
        for (const auto* s : {&d.store, &d.scratch}) {
            if (s->memory.cpu) {
                backend.FreeSurface(s->memory, s->width, s->height * (s->stencil ? 5 : 4) / 4);
            }
        }
        if (d.words.cpu) {
            backend.FreeSurface(d.words, d.store.width, WordsRows(d));
        }
    }
    depth_stores.clear();
    clear_count = 0;
    for (const auto& c : state.clears) {
        if (clear_count < MaxClears) {
            cleared[clear_count++] = {c.start, c.end, c.value, c.bytes, c.order};
        }
    }
    clear_order = state.clear_order;
    // An older capture's colour surfaces are not restored: guest memory is the picture, and the
    // frame's loads (Mem::Colour) restore what its passes start from.
    for (const auto& d : state.depths) {
        DepthStore ds{d.addr, d.width, d.height, d.format};
        ds.store = NewDepthStore(d.width, d.height, d.format);
        const u32 bytes = ds.store.width * ds.store.height * (ds.store.stencil ? 5 : 4);
        ds.valid = d.valid && bytes == d.bytes;
        // Whether guest memory took it is not recorded: as older, a read writes it back (a
        // valid store is never older than the guest's bytes).
        ds.SetGuestStale(ds.valid);
        if (ds.valid) {
            std::memcpy(ds.store.memory.cpu, d.samples, bytes);
        }
        depth_stores.push_back(ds);
    }
    // Every texture is untiled again from the guest memory the replay's loads restore. Without
    // `retile` (the replay's `profile`), textures stay as the game's do from frame to frame:
    // the loads mark those whose memory they change (ReplayMemoryWritten), and the frame's own
    // GPU writes mark theirs.
    if (retile) {
        TexturesWritten(0, 0xFFFFFFFFu);
    }
}

bool RasterizerSgxd::ReplayReadSurface(PAddr addr, std::vector<u8>& out, u32& width, u32& height) {
    backend.RenderIdle("vprof replay readback");
    const auto it = drawn_buffers.find(addr);
    if (it == drawn_buffers.end()) {
        return false;
    }
    const ColourBuffer& c = it->second;
    width = c.width;
    height = c.height;
    out = DecodeColour(memory.GetPhysicalPointer(c.addr), c.width, c.height, c.format);
    return true;
}

// vprof: the pass's colour buffer as it ended (the recorded frame's passes, and a replay's
// `results`, run synchronously), decoded from guest memory as the surfaces held it.
void RasterizerSgxd::VprofPassResult(const ColourBuffer& c) {
    if (!vprof_frame_start && Vprof::WantResults()) {
        backend.RenderIdle("vprof: the pass's result");
        const std::vector<u8> pixels =
            DecodeColour(memory.GetPhysicalPointer(c.addr), c.width, c.height, c.format);
        Vprof::FrameResult(0, c.addr, c.width, c.height, 0, pixels.data(),
                           static_cast<u32>(pixels.size()));
    }
}

// vprof: a display transfer's output as it landed in guest memory.
void RasterizerSgxd::VprofTransferResult(PAddr output, u32 width, u32 height, u32 format,
                                        const u8* out, u32 bytes) {
    if (Vprof::WantResults()) {
        backend.RenderIdle("vprof: the transfer's result");
        Vprof::FrameResult(1, output, width, height, format, out, bytes);
    }
}

bool RasterizerSgxd::ReplaySkipsDraw() {
    if (replay_limit == -2) {
        return false;
    }
    const u32 index = replay_draw_index++;
    return replay_limit >= 0 && index > static_cast<u32>(replay_limit);
}

// perflog: a transfer whose 3D took over 4 ms.
void RasterizerSgxd::PerflogTransfer(PAddr input, u32 in_width, u32 in_height, PAddr output,
                                    u32 width, u32 height, u32 scale_x, u32 scale_y, u32 ta_us,
                                    u32 d3_us) {
    static u32 slow_transfers;
    if (d3_us > 4000 && slow_transfers < 24 && SgxdFlag("perflog")) {
        slow_transfers++;
        LOG_INFO(Render,
                 "perf: slow transfer {}: {:#x} {}x{} to {:#x} {}x{} (scale {}x{}), TA {} us, 3D "
                 "{} us",
                 transfers, input, in_width, in_height, output, width, height, scale_x, scale_y,
                 ta_us, d3_us);
    }
}

// perflog: a pass's first three draws, noted with the pass.
void RasterizerSgxd::PerflogDraw(const Describe& describe, u32 isp, bool blend,
                                const SgxdRecipeDrawPrograms& p) {
    if (backend.PassDraws() <= 3 && SgxdFlag("perflog")) {
        std::string units;
        const auto textures = regs.texturing.GetTextures();
        for (u32 k = 0; k < textures.size(); k++) {
            if (textures[k].enabled) {
                units += fmt::format(" t{} {:#x} {}x{} f{}", k,
                                     textures[k].config.GetPhysicalAddress(),
                                     textures[k].config.width.Value(),
                                     textures[k].config.height.Value(),
                                     static_cast<u32>(textures[k].format));
            }
        }
        backend.NotePass(fmt::format(" [draw {}: {}, ISP {:#x}, blend {}, viewport {} {} {} {};{}]",
                                     backend.PassDraws(), describe(), isp, blend, p.viewport[0],
                                     p.viewport[1], p.viewport[2], p.viewport[3], units));
    }
}

} // namespace SgxdRenderer

#endif
