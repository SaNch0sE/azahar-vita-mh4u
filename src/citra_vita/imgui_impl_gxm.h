// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Dear ImGui's rendering half through GXM (gxm_present), for the notices drawn before sgxd takes
// the GPU (ShowGxmNotice). The menus after it draw through imgui_impl_sgxd; the input is
// imgui_impl_vita's either way.

struct ImDrawData;

/// The programs and the font atlas, into the current context. Needs GxmPresent initialised.
bool ImGui_ImplGxm_Init();
void ImGui_ImplGxm_Shutdown();
/// Between GxmPresent::BeginFrame and EndFrame.
void ImGui_ImplGxm_RenderDrawData(ImDrawData* draw_data);
