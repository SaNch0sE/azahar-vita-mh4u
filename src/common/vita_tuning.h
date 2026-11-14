// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// Switches the Vita frontend sets at startup for code outside video_core (which has GxmFlag).

namespace Common {

/// HLE DSP: the odd sources' Tick on a second thread (audio_core/hle/hle.cpp). `nodspthread`.
inline bool dsp_parallel = false;

/// Y2R: the fused conversion for MH4U's video format (core/hw/y2r.cpp). `noy2rfast`; `y2rcheck`
/// runs it and the general one and compares.
inline bool y2r_fast = true;
inline bool y2r_check = false;
/// HLE DSP ticks pipelined on a worker: the frame is computed beside the guest and output,
/// with its interrupt, half an audio frame later (audio_core/hle/hle.cpp). `nodsppipe` turns it off.
inline bool dsp_pipelined = false;

/// GX command lists on their own thread beside the guest (video_core/gpu.cpp GxLoop). `gxasync`.
inline bool gx_async = false;
/// With gx_async: a list that ends with its P3D request gets the interrupt when it is handed
/// over, not when it has been parsed (video_core/gpu.cpp). `gxearlyp3d`.
inline bool gx_early_p3d = false;

/// Vertex cache inserts only for sources already armed in the guest write tracker, verified
/// after the copy (pica/vertex_cache.h) - required once GX runs beside the guest. `vcarmfirst`.
inline bool vcache_arm_first = false;

} // namespace Common
