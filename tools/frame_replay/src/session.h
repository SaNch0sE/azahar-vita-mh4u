// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// frame_replay: a capture and everything worked out from it (cli.cpp shows it).
#pragma once

#include <QImage>
#include <QString>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "capture.h"

namespace frame_replay {

/// A picture the frame recorded for a draw: a texture, or the target at its pass's start.
struct DrawImage {
    QString title;
    QImage image; // null when not decoded
};

class Session {
public:
    std::unique_ptr<Capture> cap;

    /// Loads the capture and the replay player's results beside it (CAPTURE.player.bin).
    bool Open(const QString& path, std::string* error);

    /// The capture's info, its vblank and present intervals, the GPU's busy time.
    QString SummaryText() const;
    QString FrameText() const;
    /// The draw's state (as text), non-zero registers, shaders and uniforms.
    QString DrawText(size_t index) const;
    QString DrawShaders(size_t index) const;
    std::vector<DrawImage> DrawImages(size_t index) const;
    /// The op's memory loads: kind, address, size, blob.
    QString OpLoadsText(u32 seq) const;
    /// The replay's results as text: all passes, or one draw's cost.
    QString ReplayText(std::optional<size_t> draw) const;
    /// The console's results (PRES) against the replay's (RPRS): differing pixels and the largest
    /// channel difference for each; PNGs of both and their difference into `images` if set.
    QString CheckText(const QString& images) const;
    /// The console's results (PRES) alone, no replay needed: each pass's surface and transfer's
    /// output as sgxd made them; PNGs into `images` if set.
    QString ResultsText(const QString& images) const;
    /// The events as a Chrome trace (ui.perfetto.dev).
    std::string TraceJson() const;
};

/// HTML to plain text.
QString PlainText(const QString& html);

} // namespace frame_replay
