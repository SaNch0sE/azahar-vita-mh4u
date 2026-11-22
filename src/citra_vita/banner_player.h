// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "citra_vita/banner_scene.h"
#include "citra_vita/banner_sound.h"
#include "common/common_types.h"
#include "common/math_util.h"
#include "common/named_thread.h"

namespace Core {
class System;
}

namespace VitaFrontend {

class EmuWindowVita;

/**
 * The selected title's banner in the game list's panel, as the 3DS HOME Menu shows it: drawn by
 * azahar's own GPU with no title running (Core::System::InitGpuOnly) into the emulated top
 * screen, which the window shows in the panel (EmuWindowVita::ShowMenuBanner) under the list.
 * The GPU comes up with the list and goes down before a title loads: a title brings up its own.
 *
 * The scene is evaluated on the CPU (Banner::SceneAnimator: animation, skinning, the view and
 * projection); the PICA does the rest as it would for the HOME Menu: a vertex shader that passes
 * the vertices on, the materials' texture combiners, fragment lighting with their tables,
 * blending and depth, into a colour buffer the display transfer copies to the top screen.
 */
class BannerPlayer {
public:
    BannerPlayer(Core::System& system, EmuWindowVita& window);
    ~BannerPlayer();
    BannerPlayer(const BannerPlayer&) = delete;
    BannerPlayer& operator=(const BannerPlayer&) = delete;

    /// The GPU on its own, the banners shown in `rect` (display pixels, the 400x240 screen
    /// scaled to it).
    void Start(const Common::Rectangle<u32>& rect);
    /// The GPU down again: before a title loads, and at the app's end.
    void Stop();
    [[nodiscard]] bool Running() const {
        return running;
    }
    /// One frame of `title`'s banner (none: empty, or a title without one) on `backdrop`
    /// (0xRRGGBB), which the menus' next present shows. A new title's banner loads once the
    /// selection has rested on it for a moment, as the HOME Menu's does, on a worker (the list
    /// moves on meanwhile), and its jingle plays once from its first frame.
    void Frame(const std::string& title, u32 backdrop);

private:
    /// A title's banner as Read makes it: the scene, its textures' places in VRAM and its
    /// jingle; no scene for a title without one or one this cannot draw.
    struct Loaded {
        std::string title;
        std::unique_ptr<Banner::Scene> scene;
        std::unique_ptr<Banner::SceneAnimator> animator;
        std::map<std::string, PAddr> textures;
        u32 texture_bytes = 0;
        std::vector<s16> jingle_pcm;
    };
    /// The title's file read and its banner parsed, on the reader's thread.
    [[nodiscard]] static Loaded Read(const std::string& title);
    /// Read's banner in place of the one shown.
    void Install(Loaded&& banner);
    /// The reader's banner taken, once it is done, if it is still the one wanted.
    void TakeRead();
    void Upload();
    /// False for a frame past the banner's room in VRAM (nothing drawn).
    [[nodiscard]] bool Render(u32 backdrop);

    Core::System& system;
    EmuWindowVita& window;
    Common::Rectangle<u32> rect;
    bool running = false;
    bool shown = false;
    /// The title asked for and for how many frames; the title whose banner is loaded.
    std::string wanted;
    u32 wanted_frames = 0;
    std::string loaded;
    std::unique_ptr<Banner::Scene> scene;
    std::unique_ptr<Banner::SceneAnimator> animator;
    bool uploaded = false;
    /// Each texture's physical address in VRAM, and the bytes from the first to the last's end.
    std::map<std::string, PAddr> textures;
    u32 texture_bytes = 0;
    u32 frame = 0;
    /// The loaded banner's jingle, which starts with its first frame.
    std::vector<s16> jingle_pcm;
    Banner::JinglePlayer jingle;
    Banner::Frame scratch;
    std::vector<u32> commands;
    /// The reader: one title at a time, its banner in `read` once `read_done` (the thread last:
    /// joined before what it writes goes).
    std::atomic<bool> read_done{false};
    Loaded read;
    Common::NamedThread reader;
};

} // namespace VitaFrontend
