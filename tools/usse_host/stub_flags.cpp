#include <cstdlib>
#include <string>
#include "video_core/renderer_gxm/gxm_flags.h"
namespace GxmRenderer {
bool GxmFlag(const char* name) {
    const char* f = std::getenv("GXM_FLAGS");
    if (!f) return false;
    std::string s = std::string(" ") + f + " ";
    return s.find(std::string(" ") + name + " ") != std::string::npos;
}
bool LogtexActive() { return false; }
unsigned logtex_lines = 0;
}
