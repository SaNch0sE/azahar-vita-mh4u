// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Dear ImGui's platform half on the Vita: the pad, the front touchscreen, and a stick-driven
// cursor. The rendering half is imgui_impl_sgxd.

/// The display size and the gamepad flags, into the current context.
void ImGui_ImplVita_Init();
void ImGui_ImplVita_NewFrame();

void ImGui_ImplVita_TouchUsage(bool enabled);
void ImGui_ImplVita_GamepadUsage(bool enabled);
void ImGui_ImplVita_MouseStickUsage(bool enabled);
