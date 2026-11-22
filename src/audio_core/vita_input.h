// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "audio_core/input.h"

namespace AudioCore {

/**
 * The PS Vita's microphone (sceAudioIn): a raw port at 16 kHz mono, read by a thread of its own
 * on the miscellaneous core (sceAudioInInput blocks for one grain). Read converts what came in
 * since the last call to the game's rate, sample size and sign. A console without a microphone
 * (the PS TV) opens no port, and the game hears silence.
 */
class VitaInput final : public Input {
public:
    VitaInput();
    ~VitaInput() override;

    void StartSampling(const InputParameters& params) override;
    void StopSampling() override;
    bool IsSampling() override;
    void AdjustSampleRate(u32 sample_rate) override;
    Samples Read() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::vector<std::string> ListVitaInputDevices();

} // namespace AudioCore
