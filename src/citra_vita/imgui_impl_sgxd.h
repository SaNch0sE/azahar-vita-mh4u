// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Dear ImGui's rendering half through sgxd: each draw command becomes
// one of the recipe's draws in SgxdPresent's frame, ImGui's own fragment program
// (SGXD_IMGUI_MODULATE: the font atlas's texel times the vertex colour, blended over the
// target), with the command's clip rectangle as a tile-granular region clip.

struct ImDrawData;

/// The font atlas, to SgxdPresent's texture. Needs SgxdPresent initialised.
bool ImGui_ImplSgxd_Init();
void ImGui_ImplSgxd_Shutdown();
/// Between SgxdPresent::BeginFrame and EndFrame.
void ImGui_ImplSgxd_RenderDrawData(ImDrawData* draw_data);
