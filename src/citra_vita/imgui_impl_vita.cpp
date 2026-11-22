// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstring>

#include <imgui_vita2d/imgui.h>
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>

#include "citra_vita/imgui_impl_vita.h"
#include "citra_vita/sgxd_present.h"
#include "common/logging/log.h"

namespace {

bool EnterIsCircle() {
    static const bool circle = [] {
        SceAppUtilInitParam init{};
        SceAppUtilBootParam boot{};
        // Already up is as good (the call refuses a second init).
        sceAppUtilInit(&init, &boot);
        int enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
        if (const int got = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);
            got < 0) {
            LOG_ERROR(Frontend, "The system's enter button did not read: {:#010x}",
                      static_cast<u32>(got));
        }
        return enter == SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
    }();
    return circle;
}

using VitaFrontend::SgxdPresent::DisplayHeight;
using VitaFrontend::SgxdPresent::DisplayWidth;

struct BackendState {
    bool touch_enabled{true};
    bool gamepad_enabled{true};
    bool mouse_stick_enabled{true};
    float cursor_x{DisplayWidth / 2.0f};
    float cursor_y{DisplayHeight / 2.0f};
    /// The right stick's travel down this frame (ImGui_ImplVita_RightStickY).
    float right_y{};
    u64 last_time{};
};

/// A stick's travel from -1 to 1, 0 inside its dead zone.
float Axis(u8 raw) {
    const float value = (raw - 128.0f) / 128.0f;
    return std::abs(value) < 0.25f ? 0.0f : value;
}

BackendState s;

} // Anonymous namespace

unsigned ImGui_ImplVita_ConfirmButton() {
    return EnterIsCircle() ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
}

unsigned ImGui_ImplVita_BackButton() {
    return EnterIsCircle() ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
}

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

float ImGui_ImplVita_RightStickY() {
    return s.right_y;
}

void ImGui_ImplVita_NewFrame() {
    ImGuiIO& io = ImGui::GetIO();

    const u64 now = sceKernelGetProcessTimeWide();
    io.DeltaTime = s.last_time != 0 ? (now - s.last_time) / 1e6f : 1.0f / 60.0f;
    s.last_time = now;

    s.right_y = 0.0f;
    if (s.gamepad_enabled) {
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        std::memset(io.NavInputs, 0, sizeof(io.NavInputs));
        const auto nav = [&](ImGuiNavInput_ input, u32 button) {
            if (pad.buttons & button) {
                io.NavInputs[input] = 1.0f;
            }
        };
        nav(ImGuiNavInput_Activate, ImGui_ImplVita_ConfirmButton());
        nav(ImGuiNavInput_Cancel, ImGui_ImplVita_BackButton());
        nav(ImGuiNavInput_Menu, SCE_CTRL_SQUARE);
        nav(ImGuiNavInput_Input, SCE_CTRL_TRIANGLE);
        nav(ImGuiNavInput_DpadUp, SCE_CTRL_UP);
        nav(ImGuiNavInput_DpadDown, SCE_CTRL_DOWN);
        nav(ImGuiNavInput_DpadLeft, SCE_CTRL_LEFT);
        nav(ImGuiNavInput_DpadRight, SCE_CTRL_RIGHT);
        nav(ImGuiNavInput_FocusPrev, SCE_CTRL_LTRIGGER);
        nav(ImGuiNavInput_FocusNext, SCE_CTRL_RTRIGGER);
        // The left stick moves the selection as the d-pad does, past half its travel (ImGui's
        // own stick inputs scroll windows instead).
        const auto stick = [&](ImGuiNavInput_ input, float value) {
            if (value > 0.5f) {
                io.NavInputs[input] = 1.0f;
            }
        };
        stick(ImGuiNavInput_DpadLeft, (128.0f - pad.lx) / 128.0f);
        stick(ImGuiNavInput_DpadRight, (pad.lx - 128.0f) / 128.0f);
        stick(ImGuiNavInput_DpadUp, (128.0f - pad.ly) / 128.0f);
        stick(ImGuiNavInput_DpadDown, (pad.ly - 128.0f) / 128.0f);

        s.right_y = Axis(pad.ry);
        if (s.mouse_stick_enabled) {
            // The right stick drives a cursor, for the odd widget navigation cannot reach.
            s.cursor_x += Axis(pad.rx) * 700.0f * io.DeltaTime;
            s.cursor_y += s.right_y * 700.0f * io.DeltaTime;
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
