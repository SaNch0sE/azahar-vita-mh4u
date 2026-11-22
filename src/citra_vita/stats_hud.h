// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

/**
 * The stats overlay over a running title, top right: speed and game fps, frame time and shown
 * fps (HudStats), and for two seconds after a volume key the volume, in a translucent box. No ImGui
 * frame: its quads go straight into the present's batch, glyphs of the font atlas the present
 * samples, and are made again only when the numbers change (ImGui's frame would cost about
 * 0.25 ms a present).
 */
namespace VitaFrontend::StatsHud {

/// The characters' glyphs from the atlas's font. Once, after ImGui_ImplSgxd_Init.
void Init();

/// The overlay into the frame being built (SgxdPresent), once HudStats are valid.
void Draw();

/// Whether the speed and frame lines show (Config::Option::StatsOverlay); without them the box
/// shows only the volume, after a volume key. Any thread.
void SetShown(bool shown);

} // namespace VitaFrontend::StatsHud
