// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "citra_vita/config.h"

namespace VitaFrontend {

class EmuWindowVita;

/**
 * Registers the "vita" input engine and points the current input profile at it.
 *
 * The engine reads the pad the frontend last sampled rather than keeping any state of its own:
 * HID asks each device for its status from a core timing event, so a device that answers from
 * the latest sample is both simpler and more current than one fed press and release edges.
 */
void InitInput(EmuWindowVita& window);
void ShutdownInput();

/// The Vita input 3DS button `control` answers to (Config's Map* options, MapA's index 0): at
/// once, the guest reading its buttons from it on. Any thread.
void SetControl(int control, Config::ControlInput input);

/// Whether the 3DS's accelerometer and gyroscope read the Vita's (Config's Motion option), or
/// zero as with no motion device. At once; any thread.
void SetMotion(bool on);

/// What the right stick drives (Config's RightStick option). At once; any thread.
void SetRightStick(Config::RightStickMode mode);

/// The pad bits of a Vita input (EmuWindowVita's pad word: the rear panel's halves included), 0
/// for None.
[[nodiscard]] u32 InputMask(Config::ControlInput input);

} // namespace VitaFrontend
