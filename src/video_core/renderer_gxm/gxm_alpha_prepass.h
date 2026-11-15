// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "video_core/shader/generator/pica_fs_config.h"

namespace GxmRenderer {

/**
 * Whether a configuration's alpha test can be decided by a program that computes only the
 * combiner's alpha chain (no lighting, fog or procedural texture): 0 no alpha test, 1 yes,
 * 6 an operation, 7 a lighting colour, 8 the procedural texture, 9 a colour lane of the
 * combiner buffer, 10 a colour lane of the previous stage, 11 texture 0's type, 3 W-buffer,
 * 12 other.
 */
[[nodiscard]] int AlphaChainClass(const Pica::Shader::FSConfig& config);

/// The alpha-only form of a configuration AlphaChainClass accepts: each combiner stage's
/// colour half a copy of its alpha half, lighting, fog and the procedural texture off. Its
/// kill decision is the original's; its colour is meaningless.
[[nodiscard]] Pica::Shader::FSConfig AlphaOnlyConfig(const Pica::Shader::FSConfig& config);

} // namespace GxmRenderer
