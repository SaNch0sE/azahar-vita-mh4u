// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include "common/common_types.h"

struct SgxdRecipeDraw;

// The display through sgxd: the GPU taken from SceGpu for the
// whole session, the recipe's ImGui pass rendering each frame into one of three CDRAM display
// buffers, and SceDisplay flipping them. No GXM call may run while it is initialised: SceGpu's
// callers wait at its gate until Shutdown gives the GPU back.
namespace VitaFrontend::SgxdPresent {

inline constexpr u32 DisplayWidth = 960;
inline constexpr u32 DisplayHeight = 544;

/// Words of a vertex (ImDrawVert: x, y, u, v as f32, then the colour, A8B8G8R8).
inline constexpr u32 VertexWords = 5;

/// Takes the GPU, maps the recipe and the display buffers, and runs the init script. Returns
/// false with the reason in error; nothing is held then.
bool Initialize(std::string* error);
/// Waits for nothing (every frame is complete when EndFrame returns), shows a black frame,
/// gives the GPU back and frees the memory.
void Shutdown();
[[nodiscard]] bool IsInitialized();

/// The texture every draw samples (ImGui's font atlas): width x height texels of A8B8G8R8,
/// width a multiple of 8. Once, before the first frame: it is written into a PDS program.
bool SetTexture(const void* texels, u32 width, u32 height, bool bilinear);

/// A frame's geometry, written in place into GPU memory. Vertex n is VertexWords words at
/// vertices + n * VertexWords; the indices are 16-bit, into the frame's whole vertex array. A
/// draw's first index must be a multiple of 8. The frame's clear (draw 0, a quad over the whole
/// target) is already there when BeginFrame returns.
struct Batch {
    u32* vertices;
    u16* indices;
    SgxdRecipeDraw* draws;
    u32 vertex_count, index_count, draw_count;
    u32 max_vertices, max_indices, max_draws;
};

/// Whether the last frame's render is still on the GPU: the caller drops this frame rather
/// than wait for it (BeginFrame would).
[[nodiscard]] bool Busy();
void BeginFrame(u8 clear_r, u8 clear_g, u8 clear_b);
/// The frame being built; valid between BeginFrame and EndFrame.
[[nodiscard]] Batch& CurrentBatch();

/// Up to two guest screens a frame, drawn in the frame being built under the draws after them.
/// The framebuffer at GPU address va (its PICA physical
/// address: guest memory is mapped there) holds `columns` columns of 240 pixels, stride bytes
/// apart, in the PICA's pixel format pica_format (a Pica::PixelFormat's value); the texture unit
/// reads it as it is, and the quad turns it upright over the display rectangle (x, y, w, h). cpu,
/// the same bytes as the CPU sees them, is cleaned from its cache first; nullptr for memory the CPU
/// reaches uncached. Returns false, drawing nothing, for a format, stride or third screen it
/// does not take.
bool DrawGuestScreen(u32 va, const u8* cpu, u32 stride, u32 columns, u32 pica_format, u32 x,
                     u32 y, u32 w, u32 h);
/// Renders the batch, waits for the GPU, and queues the frame for the next vblank.
void EndFrame();

/// The next EndFrame writes its picture to ux0:data/azahar/frame_NN.ppm, as GxmPresent's does.
void DumpNextFrame();
[[nodiscard]] bool DumpPending();

} // namespace VitaFrontend::SgxdPresent
