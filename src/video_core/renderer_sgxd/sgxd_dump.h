// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace Pica {
struct ShaderSetup;
}
namespace Pica::Shader {
struct FSConfig;
}
namespace Pica::Shader::Generator {
struct PicaVSConfig;
}
namespace SgxdRenderer::Usse {
struct Program;
struct VsRequest;
} // namespace SgxdRenderer::Usse

/**
 * sgxd's dumps to host0, diagnostics builds only: sgxd_flags dumpusse's programs, declared here,
 * and the rasterizer's own (dumptex's textures, a stopped stream-out's geometry stage),
 * RasterizerSgxd's in sgxd_dump.cpp. The release build has none.
 */
namespace SgxdRenderer {

#ifdef VITA_DIAGNOSTICS
/// With sgxd_flags dumpusse, an emitted program on host0 as azahar-KIND-KEY.json
/// (Usse::ProgramJson; tools/usse2gxp.py makes a GXP of it for psp2shaderperf);
/// `kind` is vs or fs (vs2, fs2: the second tier's), `key` the program cache's.
void DumpProgram(const char* kind, u64 key, const Usse::Program& program);
/// With sgxd_flags dumpusse, the 3DS side of a fragment program: its PICA configuration's bytes
/// (azahar-fs-KEY.fsconfig) and upstream's GLSL for it (azahar-fs-KEY.glsl), for glsl2cg and
/// psp2cgc to compile the same program as Sony's compiler would.
void DumpPicaFragment(u64 key, const Pica::Shader::FSConfig& config);
/// With sgxd_flags dumpusse, the 3DS side of a vertex program: its PicaVSConfig's bytes, then
/// the PICA program code and swizzle data (4096 words each), then a trailer with what the
/// program was built for, as azahar-vs-KEY.pica (tools/pica_cg reads it), and upstream's GLSL
/// for it (azahar-vs-KEY.glsl).
void DumpPicaVertex(u64 key, const Pica::Shader::Generator::PicaVSConfig& config,
                    const Pica::ShaderSetup& setup, const Usse::VsRequest& request);
#else
inline void DumpProgram(const char*, u64, const Usse::Program&) {}
inline void DumpPicaFragment(u64, const Pica::Shader::FSConfig&) {}
inline void DumpPicaVertex(u64, const Pica::Shader::Generator::PicaVSConfig&,
                           const Pica::ShaderSetup&, const Usse::VsRequest&) {}
#endif

} // namespace SgxdRenderer
