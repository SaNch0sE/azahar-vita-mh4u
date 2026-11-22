// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// frame_replay: the PICA200's registers and textures, for the frame view.
#pragma once

#include <QImage>
#include <QString>
#include <vector>
#include "capture.h"

namespace frame_replay::pica {

/// The name of PicaCore::Regs word `index` (external or internal), or an empty string.
QString RegName(u32 index);
/// A draw's state, as HTML: target, viewport, tests, blending, textures, TEV stages, programs.
QString DescribeDraw(const Capture& cap, const Draw& d);
/// A GSP command's words, as text.
QString DescribeGsp(const GspCommand& c);

struct TextureUnit {
    bool enabled;
    u32 address, width, height, format, levels;
    bool cube;
};
TextureUnit Unit(const std::vector<u32>& regs, u32 unit);
const char* FormatName(u32 format);
/// The PICA's tiled texture (level 0) as an image, row 0 at the top as stored; null when the
/// bytes are short or the format unknown.
QImage DecodeTexture(const u8* data, size_t size, u32 width, u32 height, u32 format);
/// A colour buffer in the PICA's framebuffer layout (8x8 tiles, the colour formats).
QImage DecodeColour(const u8* data, size_t size, u32 width, u32 height, u32 format);

/// A PICA shader program's instructions from `entry` to its first end, as text.
QString Disassemble(const Program& p, u32 entry, u32 max = 512);

} // namespace frame_replay::pica
