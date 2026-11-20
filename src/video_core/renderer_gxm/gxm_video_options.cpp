// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/vita_data_dir.h"
#include "video_core/renderer_gxm/gxm_video_options.h"

namespace GxmRenderer {

namespace {

constexpr const char* OptionsPath = VITA_DATA_DIR "/video.txt";

/// The settings file's version. A file from before it (or an older one) is replaced by the
/// defaults on the first start of a release, so new defaults reach players who installed an
/// earlier build; their choices are kept from then on. Bump it when the defaults should
/// apply once more. 1: 2026-11-20 (Sharp bilinear, Smooth, HUD 1.33x, Wide, no FPS counter).
constexpr u32 OptionsVersion = 1;

void Write(const VideoOptions& o) {
    std::FILE* f = std::fopen(OptionsPath, "wb");
    if (f == nullptr) {
        return;
    }
    std::fprintf(f,
                 "version=%u\nupscale=%u\nfxaa=%u\nscale_q=%u\nmsaa=%u\nwide=%u\nfps=%u\nsmooth=%u\n"
                 "hudres=%u\n",
                 OptionsVersion, static_cast<u32>(o.upscale), o.fxaa ? 1u : 0u, o.scale_q,
                 o.msaa ? 1u : 0u, o.wide ? 1u : 0u, o.show_fps ? 1u : 0u, o.smooth ? 1u : 0u,
                 o.hudres);
    std::fclose(f);
}

VideoOptions Load(bool& reset) {
    VideoOptions o{};
    reset = false;
    std::FILE* f = std::fopen(OptionsPath, "rb");
    if (f == nullptr) {
        return o;
    }
    u32 version = 0;
    char line[128];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char* eq = std::strchr(line, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        const u32 v = static_cast<u32>(std::strtoul(eq + 1, nullptr, 10));
        if (std::strcmp(line, "version") == 0) {
            version = v;
        } else if (std::strcmp(line, "upscale") == 0 && v <= 2) {
            o.upscale = static_cast<VideoOptions::Upscale>(v);
        } else if (std::strcmp(line, "fxaa") == 0) {
            o.fxaa = v != 0;
        } else if (std::strcmp(line, "scale_q") == 0 && v >= 3 && v <= 8) {
            o.scale_q = v;
        } else if (std::strcmp(line, "msaa") == 0) {
            o.msaa = v != 0;
        } else if (std::strcmp(line, "wide") == 0) {
            o.wide = v != 0;
        } else if (std::strcmp(line, "fps") == 0) {
            o.show_fps = v != 0;
        } else if (std::strcmp(line, "smooth") == 0) {
            o.smooth = v != 0;
        } else if (std::strcmp(line, "hudres") == 0) {
            o.hudres = v <= 6 ? static_cast<u32>(v) : 0u;
        }
    }
    std::fclose(f);
    if (version != OptionsVersion) {
        // An earlier release's file: start over from the defaults (saved below).
        reset = true;
        return VideoOptions{};
    }
    return o;
}

} // Anonymous namespace

VideoOptions& Video() {
    static VideoOptions options = [] {
        bool reset = false;
        VideoOptions o = Load(reset);
        if (reset) {
            Write(o);
        }
        return o;
    }();
    return options;
}

void SaveVideoOptions() {
    Write(Video());
}

const VideoOptions& VideoAtLaunch() {
    static const VideoOptions launch = Video();
    return launch;
}

bool FilterLocked() {
    return VideoAtLaunch().hudres != 0;
}

VideoOptions::Upscale EffectiveUpscale() {
    return FilterLocked() ? VideoAtLaunch().upscale : Video().upscale;
}

bool EffectiveFxaa() {
    return FilterLocked() ? VideoAtLaunch().fxaa : Video().fxaa;
}

} // namespace GxmRenderer
