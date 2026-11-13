// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace VitaFrontend::GpuProf {

/**
 * GPU profiling on a retail console through psp2-cex-gpues4-prof (ENABLE_VITA_GPUPROF; needs
 * psp2_gpuprof.skprx under *KERNEL). Call once before sceGxmInitialize.
 *
 * From then on every wrapped GXM call is counted - presents, and scenes ended since the last
 * present - which costs nothing on the GPU. When ux0:data/mh4u/gpuprof_now appears, the next
 * point with no scene open becomes the capture's start: the identity model is seeded with
 * those exact counts (it may not be guessed for a running renderer), calls are recorded and a
 * sampler thread reads the GPU's diagnostic counters. After CaptureSeconds, recording stops
 * and ux0:data/mh4u/gpuprof.csv is written, for the repository's tools/analyze.py.
 */
void Install();

} // namespace VitaFrontend::GpuProf
