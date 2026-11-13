// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Switches the Vita frontend sets at startup for code outside video_core (which has GxmFlag).

namespace Common {

/// HLE DSP: the odd sources' Tick on a second thread (audio_core/hle/hle.cpp). `nodspthread`.
inline bool dsp_parallel = false;

/// Vertex cache inserts only for sources already armed in the guest write tracker, verified
/// after the copy (pica/vertex_cache.h) - required once GX runs beside the guest. `vcarmfirst`.
inline bool vcache_arm_first = false;

} // namespace Common
