// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

/**
 * The stats overlay over a running title, top right: speed and game fps, frame time and shown
 * fps (HudStats), in a translucent box. No ImGui frame: its quads go straight into the present's
 * batch, glyphs of the font atlas the present samples (ImGui's, the one texture of the present's
 * recipe), and are made again only when the numbers change, once a second. ImGui's frame for
 * these two lines cost about 0.25 ms a present.
 */
namespace VitaFrontend::StatsHud {

/// The characters' glyphs from the atlas's font. Once, after ImGui_ImplSgxd_Init.
void Init();

/// The overlay into the frame being built (SgxdPresent), once HudStats are valid.
void Draw();

} // namespace VitaFrontend::StatsHud
