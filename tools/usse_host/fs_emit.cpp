#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "video_core/renderer_gxm/gxm_flags.h"
#include "video_core/renderer_gxm/usse/fs_usse_gen.h"
#include "video_core/shader/generator/cg_fs_shader_gen.h"

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
namespace Pica::Shader::Generator::Cg {
DebugOutput GetDebugOutput() { return DebugOutput::None; }
bool FogDisabled() { return false; }
}

int main(int argc, char** argv) {
    alignas(16) static unsigned char raw[sizeof(Pica::Shader::FSConfig)] = {};
    auto& config = *reinterpret_cast<Pica::Shader::FSConfig*>(raw);
    std::FILE* f = std::fopen(argv[1], "rb");
    std::fprintf(stderr, "sizeof FSConfig %zu\n", sizeof(config));
    std::fread(&config, 1, sizeof(config), f);
    std::fclose(f);
    std::string refusal;
    const int level = argc > 2 ? std::atoi(argv[2]) : 2;
    auto prog = GxmRenderer::Usse::EmitFragmentProgram(config, &refusal, level);
    std::fprintf(stderr, "program %zu bytes, refusal '%s'\n", prog.size(), refusal.c_str());
}
#include "common/logging/log.h"
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char* file, unsigned int line, const char* fn,
                       fmt::string_view format, const fmt::format_args& args) {
    std::fprintf(stderr, "[log] %s:%u %s\n", file, line, fmt::vformat(format, args).c_str());
}
void Stop() {}
}
