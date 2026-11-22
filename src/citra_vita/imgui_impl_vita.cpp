// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstring>

#include <imgui_vita2d/imgui.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include "citra_vita/imgui_impl_vita.h"
#include "citra_vita/sgxd_present.h"

namespace {

using VitaFrontend::SgxdPresent::DisplayHeight;
using VitaFrontend::SgxdPresent::DisplayWidth;

struct BackendState {
    bool touch_enabled{true};
    bool gamepad_enabled{true};
    bool mouse_stick_enabled{true};
    float cursor_x{DisplayWidth / 2.0f};
    float cursor_y{DisplayHeight / 2.0f};
    u64 last_time{};
};

BackendState s;

} // Anonymous namespace

void ImGui_ImplVita_Init() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(DisplayWidth, DisplayHeight);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "imgui_impl_vita";
    s = BackendState{};
}

void ImGui_ImplVita_TouchUsage(bool enabled) {
    s.touch_enabled = enabled;
}
void ImGui_ImplVita_GamepadUsage(bool enabled) {
    s.gamepad_enabled = enabled;
}
void ImGui_ImplVita_MouseStickUsage(bool enabled) {
    s.mouse_stick_enabled = enabled;
}

void ImGui_ImplVita_NewFrame() {
    ImGuiIO& io = ImGui::GetIO();

    const u64 now = sceKernelGetProcessTimeWide();
    io.DeltaTime = s.last_time != 0 ? (now - s.last_time) / 1e6f : 1.0f / 60.0f;
    s.last_time = now;

    if (s.gamepad_enabled) {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        std::memset(io.NavInputs, 0, sizeof(io.NavInputs));
        const auto nav = [&](ImGuiNavInput_ input, u32 button) {
            if (pad.buttons & button) {
                io.NavInputs[input] = 1.0f;
            }
        };
        nav(ImGuiNavInput_Activate, SCE_CTRL_CROSS);
        nav(ImGuiNavInput_Cancel, SCE_CTRL_CIRCLE);
        nav(ImGuiNavInput_Menu, SCE_CTRL_SQUARE);
        nav(ImGuiNavInput_Input, SCE_CTRL_TRIANGLE);
        nav(ImGuiNavInput_DpadUp, SCE_CTRL_UP);
        nav(ImGuiNavInput_DpadDown, SCE_CTRL_DOWN);
        nav(ImGuiNavInput_DpadLeft, SCE_CTRL_LEFT);
        nav(ImGuiNavInput_DpadRight, SCE_CTRL_RIGHT);
        nav(ImGuiNavInput_FocusPrev, SCE_CTRL_LTRIGGER);
        nav(ImGuiNavInput_FocusNext, SCE_CTRL_RTRIGGER);

        if (s.mouse_stick_enabled) {
            // The right stick drives a cursor, for the odd widget navigation cannot reach.
            const auto axis = [](u8 raw) {
                const float value = (raw - 128.0f) / 128.0f;
                return std::abs(value) < 0.25f ? 0.0f : value;
            };
            s.cursor_x += axis(pad.rx) * 700.0f * io.DeltaTime;
            s.cursor_y += axis(pad.ry) * 700.0f * io.DeltaTime;
            s.cursor_x = std::min(std::max(s.cursor_x, 0.0f), static_cast<float>(DisplayWidth));
            s.cursor_y = std::min(std::max(s.cursor_y, 0.0f), static_cast<float>(DisplayHeight));
        }
    }

    bool touched = false;
    if (s.touch_enabled) {
        SceTouchData touch{};
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum > 0) {
            // Panel coordinates are twice the display in each axis.
            s.cursor_x = touch.report[0].x / 2.0f;
            s.cursor_y = touch.report[0].y / 2.0f;
            touched = true;
        }
    }
    io.MousePos = ImVec2(s.cursor_x, s.cursor_y);
    io.MouseDown[0] = touched;

    ImGui::NewFrame();
}
