// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Dear ImGui's platform half on the Vita: the pad, the front touchscreen, and a stick-driven
// cursor. The rendering half is imgui_impl_sgxd.

/// The display size and the gamepad flags, into the current context.
void ImGui_ImplVita_Init();
void ImGui_ImplVita_NewFrame();

/// The menus' confirm and back buttons (SCE_CTRL_*): the system's enter button setting, Circle on
/// Japanese consoles, and the other one of Cross and Circle.
unsigned ImGui_ImplVita_ConfirmButton();
unsigned ImGui_ImplVita_BackButton();

void ImGui_ImplVita_TouchUsage(bool enabled);
void ImGui_ImplVita_GamepadUsage(bool enabled);
void ImGui_ImplVita_MouseStickUsage(bool enabled);
/// The right stick's travel this frame, -1 (up) to 1 (down), 0 in its dead zone or with the
/// gamepad off; it moves the cursor as well unless MouseStickUsage turned that off.
float ImGui_ImplVita_RightStickY();
