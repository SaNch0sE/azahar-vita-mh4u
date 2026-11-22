// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <psp2/gxm.h>
#include "common/common_types.h"

/**
 * GXM for the notices before sgxd takes the GPU (ShowGxmNotice, drawn by imgui_impl_gxm): the
 * library, an immediate context, two display buffers and a shader patcher, brought up for a
 * notice and taken down after it. Everything runs on the thread that brought it up.
 */
namespace VitaFrontend::GxmPresent {

inline constexpr u32 DisplayWidth = 960;
inline constexpr u32 DisplayHeight = 544;

/// False, with the reason logged, when the console cannot present.
bool Initialize();
/// After the GPU has finished; frees everything Initialize and AllocMapped took. Before sgxd
/// takes the GPU: no GXM call may run while it has it.
void Shutdown();

void BeginFrame();
void EndFrame();

[[nodiscard]] SceGxmContext* Context();
[[nodiscard]] SceGxmShaderPatcher* Patcher();

/// GPU-visible memory (uncached LPDDR) that lives until Shutdown: the font atlas.
[[nodiscard]] void* AllocMapped(u32 size);
/// The frame's vertices and indices, between BeginFrame and EndFrame.
[[nodiscard]] void* FrameAlloc(u32 size, u32 align = 16);

} // namespace VitaFrontend::GxmPresent
