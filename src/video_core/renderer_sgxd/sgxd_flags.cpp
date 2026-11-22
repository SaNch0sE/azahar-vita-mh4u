// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>
#include "common/logging/log.h"
#include "video_core/renderer_gxm/gxm_flags.h"

namespace GxmRenderer {

namespace {

#ifdef VITA_DIAGNOSTICS
const std::vector<std::string>& Flags() {
    static const std::vector<std::string> flags = [] {
        std::vector<std::string> out;
        std::string text;
        if (const char* env = std::getenv("AZAHAR_GXM_FLAGS")) {
            // Off the console (the stub libgxm build) the flags come from the environment.
            text = env;
        }
        if (FILE* f = std::fopen("ux0:data/azahar/gxm_flags.txt", "rb")) {
            char buf[256];
            while (std::size_t n = std::fread(buf, 1, sizeof(buf), f)) {
                text.append(buf, n);
            }
            std::fclose(f);
        }
        {
            std::istringstream in(text);
            std::string word;
            while (in >> word) {
                out.push_back(word);
            }
            std::string joined;
            for (const auto& w : out) {
                joined += w + ' ';
            }
            LOG_WARNING(Render, "gxm_flags.txt: {}", joined);
        }
        return out;
    }();
    return flags;
}
#endif

} // Anonymous namespace

bool GxmFlag(const char* name) {
#ifdef VITA_DIAGNOSTICS
    for (const auto& flag : Flags()) {
        if (flag == name) {
            return true;
        }
    }
#else
    // The release build has no bring-up switches. Nothing reads the file, nothing reads the
    // environment, and every GxmFlag call site folds away to the default behaviour.
    (void)name;
#endif
    return false;
}

} // namespace GxmRenderer
