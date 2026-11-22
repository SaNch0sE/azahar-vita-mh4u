// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/named_thread.h"

namespace VitaFrontend::Banner {

/// The banner's jingle (its CWAV: DSP-ADPCM, PCM16 or PCM8, one channel or two) as 48 kHz
/// stereo frames; empty when there is none, or it does not decode (damaged, or longer than ten
/// seconds).
std::vector<s16> DecodeJingle(std::span<const u8> cwav);

/// Plays a jingle once, as the HOME Menu does when a title is selected: on an audio port of its
/// own, from a thread of its own, until it ends or Stop.
class JinglePlayer {
public:
    JinglePlayer() = default;
    ~JinglePlayer();
    JinglePlayer(const JinglePlayer&) = delete;
    JinglePlayer& operator=(const JinglePlayer&) = delete;

    /// `frames` (48 kHz stereo) from the start; the one playing stops first.
    void Play(std::vector<s16> frames);
    /// The jingle cut off: its thread told to end, which it does within one buffer, without the
    /// caller waiting (the list's selection moving on).
    void Stop();
    /// The jingle cut off and its port released (before a title opens its own).
    void StopAndWait();

private:
    std::vector<s16> pcm;
    Common::NamedThread thread;
};

} // namespace VitaFrontend::Banner
