// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <atomic>
#include <memory>
#include <numbers>

#include <psp2/ctrl.h>
#include <psp2/motion.h>

#include "citra_vita/emu_window_vita.h"
#include "citra_vita/input_factory_vita.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/vector_math.h"
#include "core/frontend/input.h"

namespace VitaFrontend {

namespace {

constexpr char EngineName[] = "vita";

EmuWindowVita* g_window = nullptr;

/**
 * The Vita pad bits each guest button answers to, in NativeButton order (SetControl's, from the
 * settings). The defaults lay the face buttons out by position rather than by name: the 3DS's A
 * is where the Vita's circle is. ZL and ZR are the rear panel's halves (EmuWindowVita::ReadPad).
 * The debug pad, Home and Power stay unbound.
 */
std::array<std::atomic<u32>, Settings::NativeButton::NumButtons> button_masks{{
    SCE_CTRL_CIRCLE,                // A
    SCE_CTRL_CROSS,                 // B
    SCE_CTRL_TRIANGLE,              // X
    SCE_CTRL_SQUARE,                // Y
    SCE_CTRL_UP,                    // Up
    SCE_CTRL_DOWN,                  // Down
    SCE_CTRL_LEFT,                  // Left
    SCE_CTRL_RIGHT,                 // Right
    SCE_CTRL_LTRIGGER,              // L
    SCE_CTRL_RTRIGGER,              // R
    SCE_CTRL_START,                 // Start
    SCE_CTRL_SELECT,                // Select
    0u,                             // Debug
    0u,                             // Gpio14
    EmuWindowVita::RearLeftButton,  // ZL
    EmuWindowVita::RearRightButton, // ZR
    0u,                             // Home
    0u,                             // Power
}};

std::atomic<Config::RightStickMode> right_stick{Config::RightStickMode::CStick};

/// Whether the right stick, in its L and R or d-pad mode, presses NativeButton `button`: past half
/// way in the button's direction.
bool RightStickPresses(std::size_t button) {
    using Mode = Config::RightStickMode;
    const Mode mode = right_stick.load(std::memory_order_relaxed);
    if (mode == Mode::CStick || g_window == nullptr) {
        return false;
    }
    constexpr float Half = 0.5f;
    const auto [x, y] = g_window->RightStick();
    switch (button) {
    case Settings::NativeButton::L:
        return mode == Mode::ShoulderButtons && x < -Half;
    case Settings::NativeButton::R:
        return mode == Mode::ShoulderButtons && x > Half;
    case Settings::NativeButton::Up:
        return mode == Mode::DPad && y > Half;
    case Settings::NativeButton::Down:
        return mode == Mode::DPad && y < -Half;
    case Settings::NativeButton::Left:
        return mode == Mode::DPad && x < -Half;
    case Settings::NativeButton::Right:
        return mode == Mode::DPad && x > Half;
    default:
        return false;
    }
}

/// A guest button, answered from the frontend's most recent pad sample through its binding.
class VitaButton final : public Input::ButtonDevice {
public:
    explicit VitaButton(std::size_t button_) : button{button_} {}

    bool GetStatus() const override {
        const u32 mask = button_masks[button].load(std::memory_order_relaxed);
        if (g_window == nullptr) {
            return false;
        }
        // The control layout's on-screen button for it and the right stick's mode, as well as
        // its input.
        return (mask != 0 && (g_window->GuestButtons() & mask) != 0) ||
               (g_window->OnScreenButtons() >> button & 1) != 0 || RightStickPresses(button);
    }

private:
    std::size_t button;
};

class VitaButtonFactory final : public Input::Factory<Input::ButtonDevice> {
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<VitaButton>(static_cast<std::size_t>(params.Get("button", 0)));
    }
};

/// The circle pad from the left stick, the C-stick from the right one (in its C-stick mode).
class VitaAnalog final : public Input::AnalogDevice {
public:
    explicit VitaAnalog(bool right_) : right{right_} {}

    std::tuple<float, float> GetStatus() const override {
        if (g_window == nullptr || (right && right_stick.load(std::memory_order_relaxed) !=
                                                 Config::RightStickMode::CStick)) {
            return {0.0f, 0.0f};
        }
        return right ? g_window->RightStick() : g_window->LeftStick();
    }

private:
    bool right;
};

class VitaAnalogFactory final : public Input::Factory<Input::AnalogDevice> {
public:
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<VitaAnalog>(params.Get("stick", "left") == "right");
    }
};

std::atomic<bool> motion_on{true};
/// SceMotion samples from the first status a guest asks for (a title that enables a sensor) on.
std::atomic<bool> motion_sampling{false};

/**
 * The 3DS's accelerometer and gyroscope from the Vita's (SceMotion's filtered state), in
 * Input::MotionDevice's frame: x+ the d-pad's left, y+ out of the touch screen, z+ the d-pad's
 * up. The Vita held in landscape is the 3DS held open, its screen the touch screen: SceMotion's
 * x (to the screen's right), y (to its top edge) and z (out of it) are the 3DS's -x, z and y,
 * one rotation for both vectors.
 * Both report what the 3DS's do: the acceleration with gravity in g (lying face up, (0, -1, 0)),
 * the angular velocity in degrees a second (SceMotion's radians), its bias corrected.
 */
class VitaMotion final : public Input::MotionDevice {
public:
    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        if (!motion_on.load(std::memory_order_relaxed)) {
            return {{}, {}};
        }
        if (!motion_sampling.exchange(true, std::memory_order_relaxed)) {
            // A plugin may sample in this process already (the start answers
            // ALREADY_SAMPLING); the state reads the same.
            const int started = sceMotionStartSampling();
            ASSERT_MSG(started >= 0 ||
                           static_cast<u32>(started) == SCE_MOTION_ERROR_ALREADY_SAMPLING,
                       "motion: sceMotionStartSampling: {:#010x}", static_cast<u32>(started));
        }
        SceMotionState state{};
        const int read = sceMotionGetState(&state);
        ASSERT_MSG(read >= 0, "motion: sceMotionGetState: {:#010x}", static_cast<u32>(read));
        const auto to_3ds = [](const SceFVector3& v) { return Common::MakeVec(-v.x, v.z, v.y); };
        constexpr float DegreesPerRadian = 180.0f / std::numbers::pi_v<float>;
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO(Input,
                     "motion: SceMotion acceleration ({:.3f}, {:.3f}, {:.3f}) g, angular velocity "
                     "({:.3f}, {:.3f}, {:.3f}) rad/s, basic orientation ({:.0f}, {:.0f}, {:.0f})",
                     state.acceleration.x, state.acceleration.y, state.acceleration.z,
                     state.angularVelocity.x, state.angularVelocity.y, state.angularVelocity.z,
                     state.basicOrientation.x, state.basicOrientation.y, state.basicOrientation.z);
        }
        return {to_3ds(state.acceleration), to_3ds(state.angularVelocity) * DegreesPerRadian};
    }
};

class VitaMotionFactory final : public Input::Factory<Input::MotionDevice> {
public:
    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<VitaMotion>();
    }
};

/// The Map* options' buttons, in their order, as NativeButtons.
constexpr std::array<Settings::NativeButton::Values, 14> MappedButtons = {
    Settings::NativeButton::A,     Settings::NativeButton::B,      Settings::NativeButton::X,
    Settings::NativeButton::Y,     Settings::NativeButton::Up,     Settings::NativeButton::Down,
    Settings::NativeButton::Left,  Settings::NativeButton::Right,  Settings::NativeButton::L,
    Settings::NativeButton::R,     Settings::NativeButton::ZL,     Settings::NativeButton::ZR,
    Settings::NativeButton::Start, Settings::NativeButton::Select,
};

/// The pad bits of each ControlInput.
constexpr std::array<u32, static_cast<std::size_t>(Config::ControlInput::Count)> InputMasks = {
    SCE_CTRL_CIRCLE,
    SCE_CTRL_CROSS,
    SCE_CTRL_TRIANGLE,
    SCE_CTRL_SQUARE,
    SCE_CTRL_UP,
    SCE_CTRL_DOWN,
    SCE_CTRL_LEFT,
    SCE_CTRL_RIGHT,
    SCE_CTRL_LTRIGGER,
    SCE_CTRL_RTRIGGER,
    SCE_CTRL_START,
    SCE_CTRL_SELECT,
    EmuWindowVita::RearLeftButton,
    EmuWindowVita::RearRightButton,
    0u,
};

} // Anonymous namespace

void SetControl(int control, Config::ControlInput input) {
    ASSERT_MSG(control >= 0 && static_cast<std::size_t>(control) < MappedButtons.size() &&
                   input >= Config::ControlInput{} && input < Config::ControlInput::Count,
               "controls: 3DS button {} to input {}", control, static_cast<int>(input));
    button_masks[MappedButtons[static_cast<std::size_t>(control)]].store(
        InputMasks[static_cast<std::size_t>(input)], std::memory_order_relaxed);
}

void SetMotion(bool on) {
    motion_on.store(on, std::memory_order_relaxed);
}

void SetRightStick(Config::RightStickMode mode) {
    right_stick.store(mode, std::memory_order_relaxed);
}

u32 InputMask(Config::ControlInput input) {
    ASSERT_MSG(input >= Config::ControlInput{} && input < Config::ControlInput::Count,
               "controls: input {}", static_cast<int>(input));
    return InputMasks[static_cast<std::size_t>(input)];
}

void InitInput(EmuWindowVita& window) {
    g_window = &window;
    Input::RegisterFactory<Input::ButtonDevice>(EngineName, std::make_shared<VitaButtonFactory>());
    Input::RegisterFactory<Input::AnalogDevice>(EngineName, std::make_shared<VitaAnalogFactory>());
    Input::RegisterFactory<Input::MotionDevice>(EngineName, std::make_shared<VitaMotionFactory>());

    auto& profile = Settings::values.current_input_profile;
    for (std::size_t i = 0; i < button_masks.size(); i++) {
        Common::ParamPackage param;
        param.Set("engine", EngineName);
        param.Set("button", static_cast<int>(i));
        profile.buttons[i] = param.Serialize();
    }
    {
        Common::ParamPackage param;
        param.Set("engine", EngineName);
        profile.analogs[Settings::NativeAnalog::CirclePad] = param.Serialize();
    }
    // The right stick drives the C-stick: the Circle Pad Pro's stick (ir:USER's extra HID, which
    // a game that asks for the accessory finds attached) and the New 3DS's (ir:rst).
    {
        Common::ParamPackage param;
        param.Set("engine", EngineName);
        param.Set("stick", "right");
        profile.analogs[Settings::NativeAnalog::CStick] = param.Serialize();
    }
    // The touchscreen is the window's own device, fed from the front panel.
    profile.touch_device = "engine:emu_window";
    profile.motion_device = std::string{"engine:"} + EngineName;
}

void ShutdownInput() {
    Input::UnregisterFactory<Input::ButtonDevice>(EngineName);
    Input::UnregisterFactory<Input::AnalogDevice>(EngineName);
    Input::UnregisterFactory<Input::MotionDevice>(EngineName);
    g_window = nullptr;
}

} // namespace VitaFrontend
