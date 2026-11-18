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

VideoOptions Load() {
    VideoOptions o{};
    std::FILE* f = std::fopen(OptionsPath, "rb");
    if (f == nullptr) {
        return o;
    }
    char line[128];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char* eq = std::strchr(line, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        const u32 v = static_cast<u32>(std::strtoul(eq + 1, nullptr, 10));
        if (std::strcmp(line, "upscale") == 0 && v <= 2) {
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
        } else if (std::strcmp(line, "pvhalf") == 0) {
            o.pv_half = v != 0;
        }
    }
    std::fclose(f);
    return o;
}

} // Anonymous namespace

VideoOptions& Video() {
    static VideoOptions options = Load();
    return options;
}

void SaveVideoOptions() {
    const VideoOptions& o = Video();
    std::FILE* f = std::fopen(OptionsPath, "wb");
    if (f == nullptr) {
        return;
    }
    std::fprintf(f, "upscale=%u\nfxaa=%u\nscale_q=%u\nmsaa=%u\nwide=%u\nfps=%u\npvhalf=%u\n",
                 static_cast<u32>(o.upscale), o.fxaa ? 1u : 0u, o.scale_q, o.msaa ? 1u : 0u,
                 o.wide ? 1u : 0u, o.show_fps ? 1u : 0u, o.pv_half ? 1u : 0u);
    std::fclose(f);
}

} // namespace GxmRenderer
