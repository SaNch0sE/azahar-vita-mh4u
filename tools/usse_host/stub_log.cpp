#include <cstdio>
#include "common/logging/log.h"
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char* file, unsigned int line, const char* fn,
                       fmt::string_view format, const fmt::format_args& args) {
    std::fprintf(stderr, "[log] %s:%u %s\n", file, line, fmt::vformat(format, args).c_str());
}
void Stop() {}
}
